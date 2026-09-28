#include "termination_signal.hpp"
#include "platform/process/graceful_stop.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace acecode::platform {
namespace {
// Lock-free operations are required because the POSIX bridge runs in a signal
// handler. Admission prevents the target's event/pipe from closing in flight.
static_assert(std::atomic<TerminationSignal*>::is_always_lock_free);
static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<TerminationSignal*> process_target{nullptr};
std::atomic<unsigned> bridge_in_flight{0};
std::atomic<bool> pending_request{false};
std::mutex registration_mu;

#ifdef _WIN32
BOOL WINAPI console_termination(DWORD code) {
    switch (code) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        TerminationSignal::request_process_termination();
        return TRUE;
    default:
        return FALSE;
    }
}
#else
extern "C" void posix_termination(int) {
    const int saved_errno = errno;
    TerminationSignal::request_process_termination();
    errno = saved_errno;
}
#endif
} // namespace

struct TerminationSignal::Impl {
    bool installed = false;
#ifdef _WIN32
    HANDLE event = nullptr;
    UniqueHandle process_stop_event;
    ~Impl() { if (event) ::CloseHandle(event); }
#else
    int pipe[2]{-1, -1};
    struct sigaction previous_int{};
    struct sigaction previous_term{};
    ~Impl() {
        if (pipe[0] >= 0) ::close(pipe[0]);
        if (pipe[1] >= 0) ::close(pipe[1]);
    }
#endif
};

TerminationSignal::TerminationSignal() : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    impl_->event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->event)
        throw std::system_error(static_cast<int>(::GetLastError()),
                                std::system_category(), "termination event");
#else
    if (::pipe(impl_->pipe) != 0)
        throw std::system_error(errno, std::generic_category(), "termination pipe");
    for (const int fd : impl_->pipe) {
        const int flags = ::fcntl(fd, F_GETFL);
        const int descriptor_flags = ::fcntl(fd, F_GETFD);
        if (flags < 0 || descriptor_flags < 0 ||
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
            ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0)
            throw std::system_error(errno, std::generic_category(), "termination pipe flags");
    }
#endif
}

TerminationSignal::~TerminationSignal() {
    if (!impl_->installed) return;
    std::lock_guard<std::mutex> registration(registration_mu);
    process_target.store(nullptr);
#ifdef _WIN32
    ::SetConsoleCtrlHandler(console_termination, FALSE);
#else
    ::sigaction(SIGINT, &impl_->previous_int, nullptr);
    ::sigaction(SIGTERM, &impl_->previous_term, nullptr);
#endif
    while (bridge_in_flight.load() != 0) std::this_thread::yield();
    pending_request.store(false);
}

void TerminationSignal::install_process_handlers() {
    std::lock_guard<std::mutex> registration(registration_mu);
    if (impl_->installed) return;
    if (process_target.load())
        throw std::logic_error("process termination handlers already have an owner");
#ifdef _WIN32
    auto process_stop_event = create_process_stop_event();
    if (!process_stop_event)
        throw std::system_error(static_cast<int>(::GetLastError()),
                                std::system_category(), "process stop event");
    if (!::SetConsoleCtrlHandler(console_termination, TRUE))
        throw std::system_error(static_cast<int>(::GetLastError()),
                                std::system_category(), "termination handler");
    impl_->process_stop_event = std::move(process_stop_event);
#else
    struct sigaction action{};
    action.sa_handler = posix_termination;
    ::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGINT, &action, &impl_->previous_int) != 0)
        throw std::system_error(errno, std::generic_category(), "SIGINT handler");
    if (::sigaction(SIGTERM, &action, &impl_->previous_term) != 0) {
        const int error = errno;
        ::sigaction(SIGINT, &impl_->previous_int, nullptr);
        throw std::system_error(error, std::generic_category(), "SIGTERM handler");
    }
#endif
    impl_->installed = true;
    process_target.store(this);
    if (pending_request.exchange(false)) request();
}

void TerminationSignal::request_process_termination() noexcept {
    bridge_in_flight.fetch_add(1);
    auto* target = process_target.load();
    if (!target) {
        pending_request.store(true);
        // Close the race with installation after the first load.
        target = process_target.load();
    }
    if (target) target->request();
    bridge_in_flight.fetch_sub(1);
}

void TerminationSignal::request() noexcept {
    requested_.store(true);
#ifdef _WIN32
    ::SetEvent(impl_->event);
#else
    const char byte = 1;
    // A full pipe already represents a pending wakeup. No locking, allocation,
    // logging, or condition-variable operation occurs in the signal handler.
    const int saved_errno = errno;
    ssize_t result;
    do { result = ::write(impl_->pipe[1], &byte, 1); } while (result < 0 && errno == EINTR);
    errno = saved_errno;
#endif
}

bool TerminationSignal::wait_for(std::chrono::milliseconds timeout) {
    if (requested()) return true;
    const auto bounded = std::clamp<long long>(timeout.count(), 0, INT_MAX);
#ifdef _WIN32
    // Both handles are borrowed from this owner until all waiters join.
    const HANDLE events[]{impl_->event, impl_->process_stop_event.get()};
    const DWORD count = impl_->process_stop_event ? 2 : 1;
    const DWORD result = ::WaitForMultipleObjects(
        count, events, FALSE, static_cast<DWORD>(bounded));
    if (result < WAIT_OBJECT_0 + count)
        requested_.store(true);
#else
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(bounded);
    auto remaining = bounded;
    for (;;) {
        struct pollfd descriptor{impl_->pipe[0], POLLIN, 0};
        const int result = ::poll(&descriptor, 1, static_cast<int>(remaining));
        if (result > 0) { requested_.store(true); break; }
        if (result == 0 || errno != EINTR) break;
        remaining = std::max<long long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count());
    }
#endif
    return requested();
}
} // namespace acecode::platform
