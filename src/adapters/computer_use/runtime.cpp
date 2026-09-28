#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00
#endif
#include "runtime.hpp"
#include "config/vocab/pointer_appearance.hpp"
#include "helper_process_posix.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <cerrno>
#include <unistd.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace acecode::computer_use {
namespace {
using json = nlohmann::json;
json failure(const char* code, const std::string& message) {
    return {{"success", false}, {"output", {{"error", code}, {"message", message}}}};
}

struct Broker {
    std::atomic<bool> allowed{false};
    std::atomic<std::uint64_t> epoch{0};
    std::mutex call_mu;
    std::mutex process_mu;
    std::mutex appearance_mu;
    std::string pointer_style = pointer_appearance::kDefaultStyle;
    std::string pointer_color = pointer_appearance::kDefaultColor;
    std::string owner;
#ifdef _WIN32
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE input = nullptr;
    HANDLE output = nullptr;
    // The caller holds process_mu for every access to these handles. Revoking
    // terminates the process but does not close a handle underneath active IO.
    void terminate() {
        if (job) TerminateJobObject(job, 1);
        else if (process) TerminateProcess(process, 1);
    }
    void close() {
        terminate();
        if (process) WaitForSingleObject(process, 1000);
        for (auto h : {input, output, process, job}) if (h) CloseHandle(h);
        input = output = process = job = nullptr;
        owner.clear();
    }
#elif defined(__APPLE__)
    PosixHelperProcess worker;
    void terminate() { worker.terminate(); }
    void close() { worker.close(); owner.clear(); }
#else
    void terminate() {}
    void close() { owner.clear(); }
#endif
    ~Broker() { close(); }
};

Broker& broker() { static Broker instance; return instance; }

#ifdef _WIN32
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    HANDLE take() { const auto h = value; value = nullptr; return h; }
};

std::wstring helper_path() {
    std::vector<wchar_t> path(32768);
    const auto len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!len || len == path.size()) return {};
    std::wstring value(path.data(), len);
    const auto slash = value.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return value.substr(0, slash + 1) + L"acecode-computer-use.exe";
}

json start_worker(Broker& state, const std::string& owner) {
    const auto path = helper_path();
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return failure("COMPUTER_USE_HELPER_MISSING",
            "acecode-computer-use.exe must be installed beside the ACECode executable.");
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    Handle child_input, parent_input, parent_output, child_output, error_output;
    // Bound the payload independently of pipe capacity; writing runs on a
    // cancellable IO thread so even a helper that never reads is revocable.
    if (!CreatePipe(&child_input.value, &parent_input.value, &sa, 65536) ||
        !CreatePipe(&parent_output.value, &child_output.value, &sa, 65536) ||
        !SetHandleInformation(parent_input.value, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(parent_output.value, HANDLE_FLAG_INHERIT, 0))
        return failure("COMPUTER_USE_PIPE_ERROR", "Could not create private helper pipes.");
    error_output.value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &sa, OPEN_EXISTING, 0, nullptr);
    if (error_output.value == INVALID_HANDLE_VALUE)
        return failure("COMPUTER_USE_PIPE_ERROR", "Could not open helper diagnostics sink.");
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> attributes(bytes);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes))
        return failure("COMPUTER_USE_START_ERROR", "Could not initialize helper handle isolation.");
    struct AttributesGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~AttributesGuard() { DeleteProcThreadAttributeList(list); }
    } guard{list};
    HANDLE inherited[] = {child_input.value, child_output.value, error_output.value};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherited, sizeof(inherited), nullptr, nullptr))
        return failure("COMPUTER_USE_START_ERROR", "Could not isolate helper handles.");
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput = child_input.value;
    si.StartupInfo.hStdOutput = child_output.value;
    si.StartupInfo.hStdError = error_output.value;
    si.lpAttributeList = list;
    Handle job;
    job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    // The helper dies with ACECode; applications explicitly launched by the
    // user/model survive the control session.
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                                              &limits, sizeof(limits)))
        return failure("COMPUTER_USE_START_ERROR", "Could not create helper lifetime job.");
    PROCESS_INFORMATION pi{};
    std::wstring command = L"\"" + path + L"\"";
    if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, nullptr, &si.StartupInfo, &pi))
        return failure("COMPUTER_USE_START_ERROR", "Could not start the Windows computer use helper.");
    Handle process, thread;
    process.value = pi.hProcess;
    thread.value = pi.hThread;
    if (!AssignProcessToJobObject(job.value, process.value) ||
        ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        TerminateProcess(process.value, 1);
        return failure("COMPUTER_USE_START_ERROR", "Could not bind helper lifetime to ACECode.");
    }
    state.process = process.take();
    state.job = job.take();
    state.input = parent_input.take();
    state.output = parent_output.take();
    state.owner = owner;
    return {{"success", true}};
}
#endif
} // namespace

bool supported() {
#ifdef _WIN32
    return true;
#elif defined(__APPLE__)
    if (__builtin_available(macOS 14.0, *)) return true;
    return false;
#else
    return false;
#endif
}
bool enabled() { return supported() && broker().allowed.load(); }

void set_pointer_appearance(const std::string& style, const std::string& color) {
    const auto normalized = pointer_appearance::normalize_color(color);
    if (!pointer_appearance::valid_style(style) || !normalized)
        throw std::invalid_argument("Computer Use pointer appearance requires ace/plain and #RRGGBB");
    auto& state = broker();
    std::lock_guard<std::mutex> lock(state.appearance_mu);
    state.pointer_style = style;
    state.pointer_color = *normalized;
}

void set_enabled(bool value) {
    auto& state = broker();
    value = value && supported();
    const bool previous = state.allowed.exchange(value);
    if (previous == value) return;
    state.epoch.fetch_add(1);
    if (!value) {
        std::lock_guard<std::mutex> lock(state.process_mu);
        state.terminate();
        // An idle worker has no outstanding IO and can be reaped immediately.
        if (state.call_mu.try_lock()) {
            state.close();
            state.call_mu.unlock();
        }
    }
}

void release_session(const std::string& session_id) {
    if (session_id.empty()) return;
    auto& state = broker();
    std::lock_guard<std::mutex> lock(state.process_mu);
    if (state.owner != session_id) return;
    state.epoch.fetch_add(1);
    state.terminate();
    if (state.call_mu.try_lock()) {
        state.close();
        state.call_mu.unlock();
    }
}

void shutdown() { set_enabled(false); }

json execute(const std::string& session_id, const json& request,
             const std::atomic<bool>* abort_flag) {
    if (!enabled()) return failure("COMPUTER_USE_DISABLED", "Enable Computer Use in Settings > Tools first.");
    if (session_id.empty()) return failure("COMPUTER_USE_NO_SESSION", "Computer use requires a bound session.");
    if (!request.is_object()) return failure("COMPUTER_USE_BAD_REQUEST", "Expected an object request.");
    auto framed = request;
    framed["protocol_version"] = 1;
    // Only the host config may supply pointer appearance. Remove model values
    // before the size check, then snapshot the latest config after serialization
    // with other computer actions, immediately before dispatch to the helper.
    framed.erase("pointer_appearance");
    if (framed.dump().size() + 1 > 32768) return failure("COMPUTER_USE_BAD_REQUEST", "Request exceeds the 32 KiB limit.");
    auto& state = broker();
    const auto epoch = state.epoch.load();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::unique_lock<std::mutex> calling(state.call_mu, std::defer_lock);
    while (!calling.try_lock()) {
        {
            std::lock_guard<std::mutex> lock(state.process_mu);
            if (!state.owner.empty() && state.owner != session_id)
                return failure("COMPUTER_USE_BUSY", "Another session owns this desktop. Wait for its turn to finish.");
        }
        if (!enabled() || state.epoch.load() != epoch)
            return failure("COMPUTER_USE_REVOKED", "Desktop control was revoked; observe again.");
        if (abort_flag && abort_flag->load())
            return failure("COMPUTER_USE_CANCELLED", "Computer use cancelled.");
        if (std::chrono::steady_clock::now() >= deadline)
            return failure("COMPUTER_USE_BUSY", "Another computer use request is still running.");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    {
        std::lock_guard<std::mutex> lock(state.appearance_mu);
        framed["pointer_appearance"] = {{"style", state.pointer_style}, {"color", state.pointer_color}};
    }
    const auto wire = framed.dump() + "\n";
    if (wire.size() > 32768) return failure("COMPUTER_USE_BAD_REQUEST", "Request exceeds the 32 KiB limit.");
#ifdef _WIN32
    const auto interrupted = [&] {
        return !enabled() || state.epoch.load() != epoch || (abort_flag && abort_flag->load());
    };
    Handle write_pipe;
    {
        std::lock_guard<std::mutex> lock(state.process_mu);
        if (interrupted()) return failure("COMPUTER_USE_CANCELLED", "Computer use cancelled or disabled.");
        if (state.process && WaitForSingleObject(state.process, 0) != WAIT_TIMEOUT) state.close();
        if (!state.owner.empty() && state.owner != session_id)
            return failure("COMPUTER_USE_BUSY", "Another session owns this desktop. Wait for its turn to finish.");
        if (!state.process) {
            auto started = start_worker(state, session_id);
            if (!started.value("success", false)) return started;
        }
        if (!DuplicateHandle(GetCurrentProcess(), state.input, GetCurrentProcess(),
                             &write_pipe.value, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            state.close();
            return failure("COMPUTER_USE_DISCONNECTED", "Could not prepare helper input.");
        }
    }
    std::atomic<bool> write_done{false};
    std::atomic<bool> write_ok{false};
    std::atomic<DWORD> writer_thread_id{0};
    const auto write_handle = write_pipe.value;
    std::thread writer([&, write_handle] {
        writer_thread_id.store(GetCurrentThreadId());
        DWORD written = 0;
        write_ok.store(WriteFile(write_handle, wire.data(), static_cast<DWORD>(wire.size()),
                                 &written, nullptr) && written == wire.size());
        write_done.store(true);
    });
    struct WriterGuard {
        Broker& state;
        std::thread& writer;
        std::atomic<bool>& done;
        std::atomic<DWORD>& thread_id;
        bool completed_response = false;
        ~WriterGuard() {
            if (!completed_response && !done.load()) {
                std::lock_guard<std::mutex> lock(state.process_mu);
                state.close(); // Closing the only reader unblocks pipe IO.
                if (const auto id = thread_id.load()) {
                    Handle thread_handle;
                    thread_handle.value = OpenThread(THREAD_TERMINATE, FALSE, id);
                    if (thread_handle.value) CancelSynchronousIo(thread_handle.value);
                }
            }
            writer.join();
        }
    } writer_guard{state, writer, write_done, writer_thread_id};
    std::string response;
    while (true) {
        bool drained = false;
        {
            std::lock_guard<std::mutex> lock(state.process_mu);
            if (interrupted()) {
                state.close();
                return failure("COMPUTER_USE_CANCELLED", "Computer use cancelled or disabled; old observations are invalid.");
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                state.close();
                return failure("COMPUTER_USE_TIMEOUT", "Windows application did not respond within 20 seconds; observe again.");
            }
            if (write_done.load() && !write_ok.load()) {
                state.close();
                return failure("COMPUTER_USE_DISCONNECTED", "Helper input closed; observe again.");
            }
            DWORD available = 0;
            if (!PeekNamedPipe(state.output, nullptr, 0, nullptr, &available, nullptr)) {
                state.close();
                return failure("COMPUTER_USE_DISCONNECTED", "Helper exited; observe again.");
            }
            if (available) {
                drained = true;
                char buffer[16384];
                DWORD received = 0;
                if (!ReadFile(state.output, buffer, (std::min)(available, static_cast<DWORD>(sizeof(buffer))), &received, nullptr)) {
                    state.close();
                    return failure("COMPUTER_USE_DISCONNECTED", "Could not read helper response.");
                }
                response.append(buffer, received);
                if (response.size() > 32 * 1024 * 1024) {
                    state.close();
                    return failure("COMPUTER_USE_RESPONSE_TOO_LARGE", "Helper response exceeds the 32 MiB limit.");
                }
                const auto newline = response.find('\n');
                if (newline != std::string::npos) {
                    auto result = json::parse(response.substr(0, newline), nullptr, false);
                    if (!result.is_object() || !result.contains("success") || !result["success"].is_boolean() ||
                        !result.contains("protocol_version") || result["protocol_version"] != 1 ||
                        newline + 1 != response.size()) {
                        state.close();
                        return failure("COMPUTER_USE_PROTOCOL_ERROR", "Invalid helper response; observe again.");
                    }
                    // A complete response proves the helper consumed the
                    // request even if the writer has not published done yet.
                    writer_guard.completed_response = true;
                    return result;
                }
            } else if (WaitForSingleObject(state.process, 0) != WAIT_TIMEOUT) {
                state.close();
                return failure("COMPUTER_USE_DISCONNECTED", "Helper exited before completing the request.");
            }
        }
        if (!drained) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#elif defined(__APPLE__)
    const auto interrupted = [&] {
        return !enabled() || state.epoch.load() != epoch || (abort_flag && abort_flag->load());
    };
    {
        std::lock_guard<std::mutex> lock(state.process_mu);
        if (interrupted()) return failure("COMPUTER_USE_CANCELLED", "Computer use cancelled or disabled.");
        if (state.worker.started() && !state.worker.alive()) state.close();
        if (!state.owner.empty() && state.owner != session_id)
            return failure("COMPUTER_USE_BUSY", "Another session owns this desktop. Wait for its turn to finish.");
        if (!state.worker.started()) {
            const auto path = computer_use_helper_path();
            if (path.empty() || access(path.c_str(), X_OK) != 0)
                return failure("COMPUTER_USE_HELPER_MISSING", "Install acecode-computer-use beside the ACECode executable.");
            if (!state.worker.start(path))
                return failure("COMPUTER_USE_START_ERROR", "Could not start the macOS Computer Use helper.");
            state.owner = session_id;
        }
    }
    std::size_t sent = 0;
    std::string response;
    while (true) {
        bool progressed = false;
        {
            std::lock_guard<std::mutex> lock(state.process_mu);
            const auto stop = [&](const char* code, const char* message) {
                state.close();
                return failure(code, message);
            };
            if (interrupted())
                return stop("COMPUTER_USE_CANCELLED", "Computer use cancelled or disabled; observe again.");
            if (std::chrono::steady_clock::now() >= deadline)
                return stop("COMPUTER_USE_TIMEOUT", "Application did not respond within 20 seconds; observe again.");
            if (sent < wire.size()) {
                const auto count = state.worker.write(wire.data() + sent, wire.size() - sent);
                if (count > 0) { sent += static_cast<std::size_t>(count); progressed = true; }
                else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                    return stop("COMPUTER_USE_DISCONNECTED", "Helper input closed; observe again.");
            }
            char buffer[65536];
            const auto count = state.worker.read(buffer, sizeof(buffer));
            if (count > 0) {
                progressed = true;
                response.append(buffer, static_cast<std::size_t>(count));
                if (response.size() > 32 * 1024 * 1024)
                    return stop("COMPUTER_USE_RESPONSE_TOO_LARGE", "Helper response exceeds the 32 MiB limit.");
                const auto newline = response.find('\n');
                if (newline != std::string::npos) {
                    auto result = json::parse(response.substr(0, newline), nullptr, false);
                    if (sent != wire.size() || !result.is_object() || !result.contains("success") ||
                        !result["success"].is_boolean() || !result.contains("protocol_version") ||
                        result["protocol_version"] != 1 || newline + 1 != response.size())
                        return stop("COMPUTER_USE_PROTOCOL_ERROR", "Invalid helper response; observe again.");
                    return result;
                }
            } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                return stop("COMPUTER_USE_DISCONNECTED", "Helper exited before completing the request.");
            }
        }
        if (!progressed) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#else
    return failure("COMPUTER_USE_UNSUPPORTED", "Computer Use requires Windows or macOS 14 or later.");
#endif
}
} // namespace acecode::computer_use
