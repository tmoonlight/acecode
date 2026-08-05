#pragma once

#include <filesystem>

namespace acecode::desktop {

struct UserInstallPaths {
    std::filesystem::path home;
    std::filesystem::path applications;
    std::filesystem::path destination;
};

// Return the only supported macOS installation layout. The caller supplies
// NSHomeDirectory() as a filesystem path so this policy remains portable and
// unit-testable on every CI platform.
UserInstallPaths macos_user_install_paths(
    const std::filesystem::path& home_directory);

// Validate paths after the caller has resolved filesystem symlinks. Exact path
// equality is intentional: a redirected ~/Applications directory must not
// silently turn a per-user installation into a write somewhere else.
bool macos_user_install_destination_is_safe(
    const std::filesystem::path& resolved_home,
    const std::filesystem::path& resolved_applications,
    const std::filesystem::path& resolved_destination);

} // namespace acecode::desktop
