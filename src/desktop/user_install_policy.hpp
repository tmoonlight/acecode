#pragma once

#include <filesystem>

namespace acecode::desktop {

struct UserInstallPaths {
    std::filesystem::path home;
    std::filesystem::path applications;
    std::filesystem::path destination;
};

struct SystemInstallPaths {
    std::filesystem::path applications;
    std::filesystem::path destination;
};

enum class MacosInstallLocation {
    unsupported,
    user_applications,
    system_applications,
};

// Return the legacy per-user macOS installation layout. The caller supplies
// NSHomeDirectory() as a filesystem path so this policy remains portable and
// unit-testable on every CI platform.
UserInstallPaths macos_user_install_paths(
    const std::filesystem::path& home_directory);

// Return the conventional system-wide layout used by the public drag-install
// DMG. These paths are intentionally fixed rather than configurable.
SystemInstallPaths macos_system_install_paths();

// Validate paths after the caller has resolved filesystem symlinks. Exact path
// equality is intentional: a redirected ~/Applications directory must not
// silently turn a per-user installation into a write somewhere else.
bool macos_user_install_destination_is_safe(
    const std::filesystem::path& resolved_home,
    const std::filesystem::path& resolved_applications,
    const std::filesystem::path& resolved_destination);

// Classify an already-resolved updater destination. Only the legacy per-user
// installation and the standard /Applications installation are accepted.
MacosInstallLocation macos_self_update_install_location(
    const std::filesystem::path& resolved_home,
    const std::filesystem::path& resolved_applications,
    const std::filesystem::path& resolved_destination);

} // namespace acecode::desktop
