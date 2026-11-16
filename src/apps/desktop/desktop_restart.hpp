#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace acecode::desktop {

// Passed to the replacement process so it knows the user did not start it by
// hand and must bring its window to the front by itself.
inline constexpr const char* kRelaunchedAfterUpdateArgument = "--relaunched-after-update";

// True when argv (argv[0] is the program) carries kRelaunchedAfterUpdateArgument.
bool desktop_relaunched_after_update(const std::vector<std::string>& argv);

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

} // namespace acecode::desktop
