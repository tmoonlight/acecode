#include "unique_resources.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <cerrno>
#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

namespace acecode::platform {

void FdTraits::close(int fd) noexcept {
#ifdef _WIN32
    ::_close(fd);
#else
    ::close(fd);
#endif
}

#ifdef _WIN32
bool HandleTraits::valid(void* handle) noexcept {
    return handle && handle != INVALID_HANDLE_VALUE;
}
void HandleTraits::close(void* handle) noexcept { ::CloseHandle(handle); }
void LocalMemTraits::close(void* handle) noexcept { ::LocalFree(handle); }
void SidTraits::close(void* handle) noexcept { ::FreeSid(handle); }
#endif

bool UniqueProcess::wait_exit(int timeout_ms) noexcept {
#ifdef _WIN32
    if (!HandleTraits::valid(process_)) return true;
    return ::WaitForSingleObject(process_, static_cast<DWORD>(timeout_ms)) == WAIT_OBJECT_0;
#else
    if (process_ <= 0) return true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        int status = 0;
        const auto result = ::waitpid(process_, &status, WNOHANG);
        if (result == process_ || (result < 0 && errno == ECHILD)) {
            process_ = invalid();
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
#endif
}

void UniqueProcess::kill() noexcept {
#ifdef _WIN32
    if (HandleTraits::valid(process_)) ::TerminateProcess(process_, 0);
#else
    if (process_ > 0) ::kill(process_, SIGKILL);
#endif
}

void UniqueProcess::reset(native_type process) noexcept {
    if (process_ == process) return;
    kill();
    wait_exit(2000);
#ifdef _WIN32
    if (HandleTraits::valid(process_)) ::CloseHandle(process_);
#endif
    process_ = process;
}

} // namespace acecode::platform
