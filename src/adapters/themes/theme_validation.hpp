#pragma once

#include "theme_package.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>

namespace acecode::themes::detail {

inline constexpr std::uintmax_t kMaxPackageBytes = 16 * 1024 * 1024;
inline constexpr std::uintmax_t kMaxPreviewBytes = 256 * 1024;
inline constexpr std::uintmax_t kMaxSoundBytes = 512 * 1024;

bool hex(const nlohmann::json& value, std::size_t length, bool prefix = false);
bool version_ok(const nlohmann::json& value);
bool asset_ok(const nlohmann::json& asset, std::uintmax_t limit);
bool png(const std::string& bytes);
bool ogg_opus(const std::string& bytes);
// Archive entry names a package may contain as a sound: sound-<name>.ogg.
bool theme_sound_file_name(const std::string& file_name);
void validate_theme_files(const nlohmann::json& definition, const ThemePackageFiles& files,
                          bool decode_legacy_images = true);

} // namespace acecode::themes::detail
