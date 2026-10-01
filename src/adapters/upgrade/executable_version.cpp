#include "executable_version.hpp"
#include "platform/process/unique_resources.hpp"
#include "utils/scope_exit.hpp"
#include "utils/semver.hpp"

#include <algorithm>
#include <array>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace acecode::upgrade {
namespace {

constexpr std::size_t kMaxVersionOutput = 4096;

bool append_output(std::string& output, const char* bytes, std::size_t size,
                   std::string& error) {
    if (size > kMaxVersionOutput - output.size()) {
        error = "executable version output exceeds limit";
        return false;
    }
    output.append(bytes, size);
    return true;
}

// 超时文案带上等了多久和最常见的原因:升级日志与界面只看得到这一句,
// 原来的「probe timed out」分不清是包坏了还是被安全软件扫描拖住。
std::string probe_timeout_error(std::chrono::milliseconds timeout) {
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(timeout).count();
    return "executable version probe timed out after " +
           (seconds > 0 ? std::to_string(seconds) + "s"
                        : std::to_string(timeout.count()) + "ms") +
           " (the new executable did not exit; security software may still be "
           "scanning it)";
}

#ifdef _WIN32
bool capture_version(const std::filesystem::path& executable,
                     std::chrono::milliseconds timeout,
                     std::string& output, std::string& error) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    platform::UniqueHandle read_pipe, write_pipe, null_file;
    if (!::CreatePipe(read_pipe.put(), write_pipe.put(), &security, 0) ||
        !::SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0)) {
        error = "cannot create executable version output pipe";
        return false;
    }
    null_file.reset(::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
    if (null_file.get() == INVALID_HANDLE_VALUE) {
        error = "cannot open executable version null stream";
        return false;
    }
    SIZE_T attribute_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<unsigned char> attribute_buffer(attribute_size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer.data());
    if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) {
        error = "cannot initialize executable version process attributes";
        return false;
    }
    ScopeExit attribute_guard([attributes] { ::DeleteProcThreadAttributeList(attributes); });
    HANDLE inherited[] = {write_pipe.get(), null_file.get()};
    if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    inherited, sizeof(inherited), nullptr, nullptr)) {
        error = "cannot restrict executable version process handles";
        return false;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null_file.get();
    startup.StartupInfo.hStdError = null_file.get();
    startup.StartupInfo.hStdOutput = write_pipe.get();
    startup.lpAttributeList = attributes;
    std::wstring command = L"\"" + executable.wstring() + L"\" --version";
    PROCESS_INFORMATION child{};
    const auto directory = executable.parent_path();
    if (!::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                          CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                          nullptr, directory.c_str(), &startup.StartupInfo, &child)) {
        error = "cannot launch executable version probe: Windows error " + std::to_string(::GetLastError());
        return false;
    }
    platform::UniqueHandle process{child.hProcess}, thread{child.hThread};
    write_pipe.reset();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto drain = [&]() {
        std::array<char, 1024> bytes{};
        for (;;) {
            DWORD available = 0;
            if (!::PeekNamedPipe(read_pipe.get(), nullptr, 0, nullptr, &available, nullptr)) {
                if (::GetLastError() == ERROR_BROKEN_PIPE) return true;
                error = "cannot read executable version output";
                return false;
            }
            if (!available) return true;
            DWORD count = 0;
            if (!::ReadFile(read_pipe.get(), bytes.data(),
                            (std::min)(available, static_cast<DWORD>(bytes.size())), &count, nullptr)) {
                error = "cannot read executable version output";
                return false;
            }
            if (!append_output(output, bytes.data(), count, error)) return false;
        }
    };
    for (;;) {
        if (!drain()) break;
        const DWORD state = ::WaitForSingleObject(process.get(), 0);
        if (state == WAIT_OBJECT_0) {
            DWORD code = 1;
            if (!drain()) return false;
            if (::GetExitCodeProcess(process.get(), &code) && code == 0) return true;
            error = "executable version probe exited unsuccessfully";
            return false;
        }
        if (state == WAIT_FAILED) {
            error = "cannot wait for executable version probe";
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            error = probe_timeout_error(timeout);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::TerminateProcess(process.get(), 1);
    ::WaitForSingleObject(process.get(), 5000);
    return false;
}
#else
bool capture_version(const std::filesystem::path& executable,
                     std::chrono::milliseconds timeout,
                     std::string& output, std::string& error) {
    int descriptors[2];
    if (::pipe(descriptors) != 0) {
        error = "cannot create executable version output pipe";
        return false;
    }
    platform::UniqueFd read_pipe{descriptors[0]}, write_pipe{descriptors[1]}, null_file{::open("/dev/null", O_RDWR)};
    if (null_file.get() < 0 || ::fcntl(read_pipe.get(), F_SETFL, O_NONBLOCK) < 0 ||
        ::fcntl(read_pipe.get(), F_SETFD, FD_CLOEXEC) < 0 ||
        ::fcntl(write_pipe.get(), F_SETFD, FD_CLOEXEC) < 0) {
        error = "cannot configure executable version streams";
        return false;
    }
    const std::string native = executable.string();
    const std::string directory = executable.parent_path().string();
    const pid_t child = ::fork();
    if (child < 0) {
        error = "cannot launch executable version probe";
        return false;
    }
    if (child == 0) {
        if (::setpgid(0, 0) != 0 || ::chdir(directory.c_str()) != 0 ||
            ::dup2(null_file.get(), STDIN_FILENO) < 0 ||
            ::dup2(null_file.get(), STDERR_FILENO) < 0 ||
            ::dup2(write_pipe.get(), STDOUT_FILENO) < 0) ::_exit(126);
        ::close(read_pipe.get());
        ::close(write_pipe.get());
        if (null_file.get() > STDERR_FILENO) ::close(null_file.get());
        ::execl(native.c_str(), native.c_str(), "--version", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    write_pipe.reset();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto drain = [&]() {
        std::array<char, 1024> bytes{};
        for (;;) {
            const ssize_t count = ::read(read_pipe.get(), bytes.data(), bytes.size());
            if (count == 0 || (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) return true;
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) { error = "cannot read executable version output"; return false; }
            if (!append_output(output, bytes.data(), static_cast<std::size_t>(count), error)) return false;
        }
    };
    for (;;) {
        if (!drain()) break;
        int status = 0;
        const pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result == child) {
            if (!drain()) return false;
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
            error = "executable version probe exited unsuccessfully";
            return false;
        }
        if (result < 0 && errno != EINTR) {
            error = "cannot wait for executable version probe";
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            error = probe_timeout_error(timeout);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::kill(-child, SIGKILL);
    ::kill(child, SIGKILL);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return false;
}
#endif

} // namespace

std::optional<std::string> parse_executable_version_output(const std::string& output) {
    const auto begin = output.find_first_not_of(" \t\r\n");
    const auto end = output.find_last_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::nullopt;
    const auto text = output.substr(begin, end - begin + 1);
    const std::string prefix = "acecode v";
    if (text.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    const auto version = text.substr(prefix.size());
    if (!parse_sem_version(version)) return std::nullopt;
    return version;
}

bool verify_executable_version(const std::filesystem::path& executable,
                               const std::string& expected_version, std::string* error,
                               std::chrono::milliseconds timeout) {
    std::string output, detail;
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(executable, ec);
    bool ok = !ec && std::filesystem::is_regular_file(absolute, ec) && !ec;
    if (!ok) detail = "executable version target does not exist";
    else ok = capture_version(absolute, timeout, output, detail);
    if (ok) {
        const auto actual = parse_executable_version_output(output);
        ok = actual && *actual == expected_version;
        if (!actual) detail = "executable returned invalid ACECode version output";
        else if (!ok) detail = "executable version mismatch: expected " + expected_version + ", got " + *actual;
    }
    if (!ok && error) *error = detail + ": " + executable.u8string();
    return ok;
}

} // namespace acecode::upgrade
