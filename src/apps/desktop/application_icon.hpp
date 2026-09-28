#pragma once

#include <filesystem>

namespace acecode::desktop {

// Prefer the installed application's artwork over the launch directory.
std::filesystem::path find_application_icon_path(
    const std::filesystem::path& executable,
    const std::filesystem::path& working_directory);

std::filesystem::path application_icon_path();

#if !defined(_WIN32) && !defined(__APPLE__)
// Optional native decoration: missing artwork must not prevent startup.
bool set_linux_window_icon(void* window, const std::filesystem::path& icon_path);
#endif

} // namespace acecode::desktop
