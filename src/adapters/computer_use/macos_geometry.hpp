#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace acecode::computer_use::macos {
struct ScreenRect { double x = 0, y = 0, width = 0, height = 0; };
struct ScreenPoint { double x, y; };

inline bool valid_rect(ScreenRect rect) {
    return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) &&
        std::isfinite(rect.height) && rect.width > 0 && rect.height > 0 &&
        std::isfinite(rect.x + rect.width) && std::isfinite(rect.y + rect.height);
}
inline std::optional<ScreenPoint> image_to_screen(double x, double y, int width, int height, ScreenRect rect) {
    if (!valid_rect(rect) || width <= 0 || height <= 0 || !std::isfinite(x) || !std::isfinite(y) ||
        x < 0 || y < 0 || x >= width || y >= height) return std::nullopt;
    const ScreenPoint point{rect.x + x * rect.width / width, rect.y + y * rect.height / height};
    if (point.x >= rect.x + rect.width || point.y >= rect.y + rect.height) return std::nullopt;
    return point;
}
inline std::pair<int, int> capture_size(ScreenRect rect, double scale) {
    if (!valid_rect(rect) || !std::isfinite(scale) || scale <= 0) return {0, 0};
    const auto factor = std::min(scale, 2560.0 / std::max(rect.width, rect.height));
    return {std::max(1, static_cast<int>(std::round(rect.width * factor))),
            std::max(1, static_cast<int>(std::round(rect.height * factor)))};
}
} // namespace acecode::computer_use::macos
