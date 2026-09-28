#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>

namespace acecode::themes {

using ThemePackageFiles = std::map<std::string, std::string>;

// Writes a complete archive to a caller-owned temporary path. Progress comes
// from libzip's compression callback; cancellation never publishes the file.
void write_theme_archive(const std::filesystem::path& path,
                         const ThemePackageFiles& files,
                         const std::function<void(double)>& progress = {},
                         const std::function<bool()>& cancelled = {});

} // namespace acecode::themes
