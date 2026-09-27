#pragma once

#include <optional>
#include <string>

namespace acecode::computer_use::pointer_appearance {

inline constexpr const char* kDefaultStyle = "ace";
inline constexpr const char* kDefaultColor = "#2563eb";

inline bool valid_style(const std::string& style) {
    return style == "ace" || style == "plain";
}

// Keep the wire/config format independent from CSS parsing and locale rules.
inline std::optional<std::string> normalize_color(const std::string& color) {
    if (color.size() != 7 || color[0] != '#') return std::nullopt;
    std::string normalized = color;
    for (std::size_t index = 1; index < normalized.size(); ++index) {
        const char digit = normalized[index];
        if (digit >= 'A' && digit <= 'F') normalized[index] = static_cast<char>(digit - 'A' + 'a');
        else if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) return std::nullopt;
    }
    return normalized;
}

} // namespace acecode::computer_use::pointer_appearance
