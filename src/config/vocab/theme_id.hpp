#pragma once

#include <algorithm>
#include <string>

namespace acecode::themes {

inline constexpr char kNationalDayThemeId[] = "national-day-2026";

inline bool is_downloadable_theme(const std::string& id) {
    return id == "eva-01" || id == kNationalDayThemeId;
}

// Local themes are data packages, never arbitrary filesystem paths.
inline bool is_local_theme(const std::string& id) {
    return id.size() > 3 && id.size() <= 64 && id.compare(0, 3, "ai-") == 0 &&
        id[3] != '-' && id.back() != '-' && id.find("--") == std::string::npos &&
        std::all_of(id.begin() + 3, id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        });
}

} // namespace acecode::themes
