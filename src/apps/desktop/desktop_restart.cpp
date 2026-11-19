#include "desktop_restart.hpp"

#include "daemon_pool.hpp"
#include "ipc/runtime_files.hpp"
#include "platform/process/os_process.hpp"
#include "utils/logger.hpp"
#include "utils/state_file.hpp"
#include "utils/utf8_path.hpp"
#include "webview_health.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cstdint>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <cstring>
#  include <fcntl.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  ifdef __APPLE__
#    include <crt_externs.h>
#    include <mach-o/dyld.h>
#  endif
#endif

namespace fs = std::filesystem;

namespace acecode::desktop {
namespace {

constexpr const char* kLaunchVersionStateKey = "desktop_last_launch_version";
constexpr const char* kWebviewRecoveryRestartsStateKey = "desktop_webview_recovery_restarts";

bool has_argument(const std::vector<std::string>& argv, const char* argument) {
    return argv.size() > 1 && std::find(argv.begin() + 1, argv.end(), argument) != argv.end();
}

#ifndef _WIN32
// fork + exec. A close-on-exec pipe reports an exec failure back to the caller.
// With wait_for_exit the child is also reaped and must exit with status 0.
bool spawn_process(const std::vector<std::string>& command,
                   bool wait_for_exit,
                   std::string* error) {
    int error_pipe[2] = {-1, -1};
    if (::pipe(error_pipe) != 0) {
        if (error) *error = std::string("pipe failed: ") + std::strerror(errno);
        return false;
    }
    if (::fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC) != 0) {
        const int saved_errno = errno;
        ::close(error_pipe[0]);
        ::close(error_pipe[1]);
        if (error) *error = std::string("fcntl failed: ") + std::strerror(saved_errno);
        return false;
    }

    // Built before fork: the child may only call async-signal-safe functions.
    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (const auto& part : command) argv.push_back(const_cast<char*>(part.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int saved_errno = errno;
        ::close(error_pipe[0]);
        ::close(error_pipe[1]);
        if (error) *error = std::string("fork failed: ") + std::strerror(saved_errno);
        return false;
    }
    if (pid == 0) {
        ::close(error_pipe[0]);
        ::setsid();
        ::execv(argv[0], argv.data());
        const int saved_errno = errno;
        (void)::write(error_pipe[1], &saved_errno, sizeof(saved_errno));
        ::_exit(127);
    }

    ::close(error_pipe[1]);
    int child_errno = 0;
    ssize_t bytes = -1;
    do {
        bytes = ::read(error_pipe[0], &child_errno, sizeof(child_errno));
    } while (bytes < 0 && errno == EINTR);
    const int read_errno = errno;
    ::close(error_pipe[0]);
    if (bytes < 0) {
        if (error) *error = std::string("restart handshake failed: ") + std::strerror(read_errno);
        return false;
    }
    if (bytes > 0) {
        (void)::waitpid(pid, nullptr, 0);
        if (error) *error = std::string("exec failed: ") + std::strerror(child_errno);
        return false;
    }
    if (!wait_for_exit) return true;

    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (error) *error = command.front() + " did not exit cleanly";
        return false;
    }
    return true;
}
#endif

#ifdef __APPLE__
// LaunchServices starts the application from the login session's environment.
// A process started with ACECODE_* overrides (a development instance, keys
// exported in a shell) must keep them, so it is executed directly instead.
bool has_acecode_environment_overrides() {
    char*** environment = _NSGetEnviron();
    if (!environment || !*environment) return false;
    for (char** entry = *environment; *entry; ++entry) {
        if (std::strncmp(*entry, "ACECODE_", 8) == 0) return true;
    }
    return false;
}
#endif

} // namespace

fs::path current_desktop_executable_path() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD size = static_cast<DWORD>(buffer.size());
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), size);
        if (length == 0) return {};
        if (length < size - 1) return fs::path(std::wstring(buffer.data(), length));
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 1024;
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.assign(static_cast<std::size_t>(size) + 1, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    }
    // Capture the stable installation path before an update renames the bundle.
    std::error_code ec;
    const fs::path resolved = fs::canonical(fs::path(buffer.data()), ec);
    return ec ? fs::path{} : resolved;
#else
    std::vector<char> buffer(4096);
    for (;;) {
        const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) return {};
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return fs::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

DesktopRestartPreflight validate_desktop_restart_target(const fs::path& executable) {
    DesktopRestartPreflight result;
    if (executable.empty()) {
        result.error = "desktop executable path is empty";
        return result;
    }

    std::error_code ec;
    if (!fs::exists(executable, ec) || ec) {
        result.error = "desktop executable does not exist: " + acecode::path_to_utf8(executable);
        return result;
    }
    if (!fs::is_regular_file(executable, ec) || ec) {
        result.error = "desktop executable is not a regular file: " + acecode::path_to_utf8(executable);
        return result;
    }
#ifndef _WIN32
    if (::access(executable.c_str(), X_OK) != 0) {
        result.error = "desktop executable is not executable: " + acecode::path_to_utf8(executable);
        return result;
    }
#endif
    result.ok = true;
    return result;
}

bool desktop_relaunched_after_update(const std::vector<std::string>& argv) {
    return has_argument(argv, kRelaunchedAfterUpdateArgument);
}

bool desktop_was_relaunched(const std::vector<std::string>& argv) {
    return has_argument(argv, kRelaunchedAfterUpdateArgument) ||
           has_argument(argv, kRelaunchedAfterWebviewFailureArgument);
}

bool DesktopRestartRequest::request(DesktopRestartReason reason) {
    DesktopRestartReason expected = DesktopRestartReason::None;
    return reason != DesktopRestartReason::None &&
           reason_.compare_exchange_strong(expected, reason);
}

DesktopRestartReason DesktopRestartRequest::reason() const { return reason_.load(); }

bool DesktopRestartRequest::pending() const {
    return reason_.load() != DesktopRestartReason::None;
}

DesktopExitPlan plan_desktop_exit(DesktopRestartReason reason) {
    DesktopExitPlan plan;
    switch (reason) {
    case DesktopRestartReason::None:
        break;
    case DesktopRestartReason::Upgrade:
        plan.relaunch = true;
        plan.stop_daemons_for_upgrade = true;
        plan.relaunch_argument = kRelaunchedAfterUpdateArgument;
        plan.label = "post-upgrade";
        break;
    case DesktopRestartReason::WebviewRecovery:
        plan.relaunch = true;
        plan.keep_daemons_running = true;
        plan.relaunch_argument = kRelaunchedAfterWebviewFailureArgument;
        plan.label = "webview-recovery";
        break;
    }
    return plan;
}

bool note_webview_recovery_restart(std::int64_t now_unix_ms) {
    bool admitted = false;
    const bool saved = update_state_json([now_unix_ms, &admitted](nlohmann::json& state) {
        std::vector<std::int64_t> history;
        const auto it = state.find(kWebviewRecoveryRestartsStateKey);
        if (it != state.end() && it->is_array()) {
            for (const auto& item : *it) {
                if (item.is_number_integer()) history.push_back(item.get<std::int64_t>());
            }
        }
        admitted = admit_recovery_restart(history, now_unix_ms);
        state[kWebviewRecoveryRestartsStateKey] = history;
        return true;
    });
    return saved && admitted;
}

std::function<void(bool user_initiated)> make_webview_recovery_handler(
    DesktopRestartRequest& request,
    fs::path executable,
    std::function<void()> quit) {
    // Borrowed: the request lives in main's run scope, which outlives the GUI loop.
    DesktopRestartRequest* slot = &request;
    return [slot, executable = std::move(executable), quit = std::move(quit)](bool user_initiated) {
        if (slot->pending()) return;
        const auto preflight = validate_desktop_restart_target(executable);
        if (!preflight.ok) {
            LOG_ERROR("[desktop] webview-recovery restart preflight failed: " + preflight.error);
            return;
        }
        const auto now_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (!user_initiated && !note_webview_recovery_restart(now_unix_ms)) {
            LOG_ERROR("[desktop] webview-recovery restart skipped: too many automatic "
                      "restarts recently, or the restart record could not be saved");
            return;
        }
        if (!slot->request(DesktopRestartReason::WebviewRecovery)) return;
        LOG_WARN(std::string("[desktop] webview-recovery restart requested (") +
                 (user_initiated ? "user request" : "automatic") + ")");
        if (quit) quit();
    };
}

bool note_desktop_version_launch(const std::string& version) {
    if (version.empty()) return false;
    bool changed = false;
    (void)update_state_json([&version, &changed](nlohmann::json& state) {
        const auto it = state.find(kLaunchVersionStateKey);
        if (it != state.end() && it->is_string() && it->get<std::string>() == version) {
            return false;
        }
        changed = true;
        state[kLaunchVersionStateKey] = version;
        return true;
    });
    return changed;
}

std::optional<fs::path> macos_app_bundle_for_executable(const fs::path& executable) {
    const std::string generic = executable.generic_string();
    if (generic.empty() || generic.front() != '/') return std::nullopt;
    const fs::path normalized = executable.lexically_normal();
    const fs::path macos_dir = normalized.parent_path();
    const fs::path contents_dir = macos_dir.parent_path();
    const fs::path app_dir = contents_dir.parent_path();
    if (!normalized.has_filename() || macos_dir.filename() != "MacOS" ||
        contents_dir.filename() != "Contents" || app_dir.extension() != ".app" ||
        app_dir.stem().empty()) {
        return std::nullopt;
    }
    return app_dir;
}

bool valid_desktop_replacement_arguments(const std::vector<std::string>& arguments,
                                         std::string* error) {
    for (const auto& argument : arguments) {
        const bool plain = !argument.empty() &&
            std::all_of(argument.begin(), argument.end(), [](unsigned char ch) {
                return ch > 0x20 && ch < 0x7F && ch != 0x22 && ch != 0x5C;
            });
        if (!plain) {
            if (error) *error = "desktop replacement argument is not a plain switch";
            return false;
        }
    }
    return true;
}

bool launch_desktop_replacement(const fs::path& executable,
                                const std::vector<std::string>& arguments,
                                std::string* error) {
    const auto preflight = validate_desktop_restart_target(executable);
    if (!preflight.ok) {
        if (error) *error = preflight.error;
        return false;
    }
    if (!valid_desktop_replacement_arguments(arguments, error)) return false;

#ifdef _WIN32
    std::wstring application = executable.native();
    std::wstring command_line = L"\"" + application + L"\"";
    for (const auto& argument : arguments) {
        // Validated above: printable ASCII without spaces, quotes or backslashes.
        command_line += L' ';
        command_line.append(argument.begin(), argument.end());
    }
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const fs::path parent = executable.parent_path();
    const BOOL created = ::CreateProcessW(
        application.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, 0,
        nullptr, parent.empty() ? nullptr : parent.c_str(), &startup, &process);
    if (!created) {
        if (error) {
            *error = "CreateProcessW failed with error " +
                     std::to_string(static_cast<unsigned long>(::GetLastError()));
        }
        return false;
    }
    // Only effective while this process may still set the foreground itself;
    // otherwise the replacement forces its own window forward.
    (void)::AllowSetForegroundWindow(process.dwProcessId);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
#else
#ifdef __APPLE__
    if (const auto bundle = macos_app_bundle_for_executable(executable);
        bundle && !has_acecode_environment_overrides()) {
        // -n: this process is still registered with LaunchServices, which would
        // otherwise activate it instead of starting the replacement.
        std::vector<std::string> command{
            "/usr/bin/open", "-n", acecode::path_to_utf8(*bundle)};
        if (!arguments.empty()) {
            command.emplace_back("--args");
            command.insert(command.end(), arguments.begin(), arguments.end());
        }
        if (spawn_process(command, /*wait_for_exit=*/true, nullptr)) return true;
        // Relaunching matters more than activation: fall back to a direct exec.
    }
#endif
    std::vector<std::string> command{acecode::path_to_utf8(executable)};
    command.insert(command.end(), arguments.begin(), arguments.end());
    return spawn_process(command, /*wait_for_exit=*/false, error);
#endif
}

bool wait_for_replacement_desktop_owner(const std::string& run_dir,
                                        std::int64_t timeout_ms,
                                        std::int64_t self_pid) {
    const std::int64_t self = self_pid != 0
        ? self_pid
        : static_cast<std::int64_t>(acecode::daemon::current_pid());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const auto owner = acecode::daemon::read_desktop_owner_record(run_dir);
        if (owner && owner->pid > 0 && owner->pid != self &&
            acecode::daemon::is_pid_alive(owner->pid)) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

int finish_desktop_exit(DaemonPool& pool,
                        DesktopRestartReason reason,
                        const fs::path& executable,
                        const std::string& run_dir,
                        const std::function<void()>& release_single_instance) {
    const DesktopExitPlan plan = plan_desktop_exit(reason);
    const std::string label = plan.label;
    if (plan.relaunch) LOG_INFO("[desktop] " + label + " daemon shutdown started");
    if (plan.keep_daemons_running) pool.set_keep_alive_on_exit(true);
    const auto failures = pool.shutdown_all(plan.stop_daemons_for_upgrade
        ? DaemonShutdownReason::UpgradeRestart
        : DaemonShutdownReason::ApplicationExit);
    if (!plan.relaunch) return failures.empty() ? 0 : 100; // 部分失败返回非零便于诊断
    LOG_INFO("[desktop] " + label + " daemon shutdown finished; failures=" +
             std::to_string(failures.size()));
    for (const auto& failure : failures) {
        LOG_ERROR("[desktop] " + label + " daemon shutdown failed: " + failure.first + ": " +
                  failure.second);
    }
    if (!failures.empty() && plan.stop_daemons_for_upgrade) {
        LOG_ERROR("[desktop] upgrade restart aborted");
        return 100;
    }
    // The replacement must not see the dying process's singleton guard and
    // focus it instead of starting. At this point the WebView loop, tray,
    // notifications, and managed daemons have all completed teardown.
    if (release_single_instance) release_single_instance();
    LOG_INFO("[desktop] " + label + " replacement launch started");
    std::string restart_error;
    if (!launch_desktop_replacement(executable, {plan.relaunch_argument}, &restart_error)) {
        LOG_ERROR("[desktop] failed to launch replacement (" + label + "): " + restart_error);
        return 101;
    }
    LOG_INFO("[desktop] launched replacement (" + label + ")");
    if (plan.keep_daemons_running) {
        // 替身起得慢一些也要等:先退出的话 daemon 会当作属主没了而自行停止。
        const bool handed_over = wait_for_replacement_desktop_owner(run_dir, 20 * 1000);
        if (handed_over) {
            LOG_INFO("[desktop] replacement took over the managed daemons");
        } else {
            LOG_WARN("[desktop] replacement did not publish its owner record in time; "
                     "managed daemons may stop");
        }
    }
    return failures.empty() ? 0 : 100;
}

} // namespace acecode::desktop
