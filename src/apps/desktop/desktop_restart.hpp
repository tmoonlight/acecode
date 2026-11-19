#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace acecode::desktop {

class DaemonPool;

// Passed to the replacement process so it knows the user did not start it by
// hand and must bring its window to the front by itself.
inline constexpr const char* kRelaunchedAfterUpdateArgument = "--relaunched-after-update";

// True when argv (argv[0] is the program) carries kRelaunchedAfterUpdateArgument.
bool desktop_relaunched_after_update(const std::vector<std::string>& argv);

// Passed to the replacement started after the main WebView became unusable
// (its browser process exited). Like the update switch it makes the replacement
// bring its window forward: the restart only happens while the user is at the
// window, or asked for it from the tray.
inline constexpr const char* kRelaunchedAfterWebviewFailureArgument =
    "--relaunched-after-webview-failure";

// True when argv carries either relaunch switch.
bool desktop_was_relaunched(const std::vector<std::string>& argv);

// Why the running process is going to be replaced once its GUI loop returns.
enum class DesktopRestartReason {
    None,
    Upgrade,          // the updater installed a new build
    WebviewRecovery,  // the main WebView is gone and cannot be repaired in place
};

// Written on the GUI thread, read by the exit path after the GUI loop returns.
class DesktopRestartRequest {
public:
    // The first reason wins; returns false when a restart is already pending.
    bool request(DesktopRestartReason reason);
    DesktopRestartReason reason() const;
    bool pending() const;

private:
    std::atomic<DesktopRestartReason> reason_{DesktopRestartReason::None};
};

// What the exit path does for each restart reason.
struct DesktopExitPlan {
    bool relaunch = false;
    // An upgrade must see every managed daemon exit (they run the old build)
    // and gives up otherwise. A WebView recovery leaves them running for the
    // replacement to attach to, so turns in progress are not interrupted.
    bool stop_daemons_for_upgrade = false;
    bool keep_daemons_running = false;
    const char* relaunch_argument = "";
    const char* label = "";  // log wording
};
DesktopExitPlan plan_desktop_exit(DesktopRestartReason reason);

// Cross-process limit for automatic WebView recovery restarts, kept in
// state.json (see admit_recovery_restart). Returns false when the limit is
// reached or the record cannot be saved: without a record a browser process
// that dies on every start would restart the application forever.
bool note_webview_recovery_restart(std::int64_t now_unix_ms);

// The callback for WebHost::set_webview_recovery_handler: checks the restart
// target and the automatic-restart limit (skipped for a user request), records
// the reason in `request` and calls `quit`. `request` must outlive the GUI loop.
std::function<void(bool user_initiated)> make_webview_recovery_handler(
    DesktopRestartRequest& request,
    std::filesystem::path executable,
    std::function<void()> quit);

// Record the running version in state.json. Returns true when the previous
// launch recorded a different version or none at all: the first launch of a
// newly installed build, including one relaunched by an older build that does
// not pass kRelaunchedAfterUpdateArgument yet.
bool note_desktop_version_launch(const std::string& version);

// The enclosing <name>.app for .../<name>.app/Contents/MacOS/<executable>.
// Portable so the layout rule is covered by the normal unit suite.
std::optional<std::filesystem::path> macos_app_bundle_for_executable(
    const std::filesystem::path& executable);

// Replacement arguments are internal switches. Anything that would need
// quoting on a command line is rejected instead of being escaped.
bool valid_desktop_replacement_arguments(const std::vector<std::string>& arguments,
                                         std::string* error = nullptr);

struct DesktopRestartPreflight {
    bool ok = false;
    std::string error;
};

// Resolve this process's installed executable path. Call this before an update
// can rename the running image into its backup directory.
std::filesystem::path current_desktop_executable_path();

// Check the immutable install path before accepting a restart request.
DesktopRestartPreflight validate_desktop_restart_target(
    const std::filesystem::path& executable);

// Launch the replacement desktop process. The caller must first tear down its
// daemon/tray resources and release the desktop single-instance guard.
//
// By the time this runs the caller has destroyed its window and is no longer
// the foreground application, so the replacement cannot take the foreground
// the way a hand-started process can:
//   - Windows: the replacement is granted the foreground right when this
//     process still holds it; otherwise it forces its own window to the front
//     (see window_foreground.hpp) because `arguments` tells it to.
//   - macOS: an installed bundle is relaunched through LaunchServices
//     (/usr/bin/open -n), which activates the new instance. A bare executable,
//     or a process carrying ACECODE_* environment overrides that LaunchServices
//     would drop, is executed directly as before.
bool launch_desktop_replacement(const std::filesystem::path& executable,
                                const std::vector<std::string>& arguments,
                                std::string* error);

// Block until another live Desktop process has published its owner record in
// `run_dir`, or the timeout passes. A managed daemon that sees its owner die
// waits well under a second for a new owner and then stops itself unless the
// user enabled "keep running after exit" (apps/daemon/worker.cpp), so a process
// that hands its daemons to a replacement must outlive that hand-over.
// `self_pid` is this process (0 = ask the OS); tests pass another value.
bool wait_for_replacement_desktop_owner(const std::string& run_dir,
                                        std::int64_t timeout_ms,
                                        std::int64_t self_pid = 0);

// The tail of the desktop process: shut the managed daemons down as the restart
// reason requires and, for a restart, release the single-instance guard and
// launch the replacement. Call after the GUI loop, tray and notifications are
// gone. `run_dir` is the shared daemon runtime directory this process published
// its owner record in. Returns the process exit code.
int finish_desktop_exit(DaemonPool& pool,
                        DesktopRestartReason reason,
                        const std::filesystem::path& executable,
                        const std::string& run_dir,
                        const std::function<void()>& release_single_instance);

} // namespace acecode::desktop
