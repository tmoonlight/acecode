#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

namespace acecode::computer_use::coordinate_transform {

struct Point {
    std::int32_t x;
    std::int32_t y;
};

struct Bounds {
    std::int32_t left;
    std::int32_t top;
    std::int32_t right;
    std::int32_t bottom;
};

// Bounds belong to the selected screenshot, in physical desktop pixels.
// Screenshot and desktop bounds are half-open; preserve the existing floor rule.
inline std::optional<Point> screenshot_to_desktop(double x, double y, Bounds bounds,
                                                  std::int32_t width, std::int32_t height) {
    const auto native_width = static_cast<std::int64_t>(bounds.right) - bounds.left;
    const auto native_height = static_cast<std::int64_t>(bounds.bottom) - bounds.top;
    if (width <= 0 || height <= 0 || native_width <= 0 || native_height <= 0
        || !std::isfinite(x) || !std::isfinite(y)
        || x < 0 || y < 0 || x >= width || y >= height) return std::nullopt;

    const double offset_x = std::floor(x * static_cast<double>(native_width) / width);
    const double offset_y = std::floor(y * static_cast<double>(native_height) / height);
    // Do not let floating-point rounding at an edge produce a point outside it.
    if (offset_x < 0 || offset_y < 0 || offset_x >= native_width || offset_y >= native_height)
        return std::nullopt;
    return Point{
        static_cast<std::int32_t>(static_cast<std::int64_t>(bounds.left) + static_cast<std::int64_t>(offset_x)),
        static_cast<std::int32_t>(static_cast<std::int64_t>(bounds.top) + static_cast<std::int64_t>(offset_y))};
}

// Preserve SendInput's existing 65536-cell center formula, not 65535/(size-1).
inline std::optional<Point> desktop_to_absolute(Point point, Point origin,
                                               std::int32_t width, std::int32_t height) {
    const auto relative_x = static_cast<std::int64_t>(point.x) - origin.x;
    const auto relative_y = static_cast<std::int64_t>(point.y) - origin.y;
    if (width <= 0 || height <= 0
        || relative_x < 0 || relative_y < 0 || relative_x >= width || relative_y >= height)
        return std::nullopt;
    return Point{
        static_cast<std::int32_t>((relative_x * 65536 + 32768) / width),
        static_cast<std::int32_t>((relative_y * 65536 + 32768) / height)};
}

} // namespace acecode::computer_use::coordinate_transform
