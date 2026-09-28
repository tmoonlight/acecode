#include "window_chrome.hpp"

#include <algorithm>
#include <cmath>

namespace acecode::desktop {

namespace {

constexpr std::uint32_t kWindows10FirstBuild = 10240;
constexpr std::uint32_t kWindows10Version1809Build = 17763;
constexpr std::uint32_t kWindows11FirstBuild = 22000;

std::uint8_t blend_channel(std::uint8_t foreground,
                           std::uint8_t background,
                           double foreground_weight) {
    const double value = foreground * foreground_weight +
                         background * (1.0 - foreground_weight);
    return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
}

RgbColor blend(RgbColor foreground, RgbColor background, double foreground_weight) {
    return RgbColor{
        blend_channel(foreground.r, background.r, foreground_weight),
        blend_channel(foreground.g, background.g, foreground_weight),
        blend_channel(foreground.b, background.b, foreground_weight),
    };
}

} // namespace

int frameless_resize_border(int frame, int padding) {
    return std::max(1, frame + padding);
}

FramelessHitTestArea classify_frameless_hit_test(const FramelessHitTestInput& input) {
    if (input.width <= 0 || input.height <= 0) return FramelessHitTestArea::Client;

    const int border_x = frameless_resize_border(input.frame_x, input.padding);
    const int border_y = frameless_resize_border(input.frame_y, input.padding);
    const int drag_height = std::max(1, input.drag_height);

    if (!input.maximized) {
        const bool left = input.x >= 0 && input.x < border_x;
        const bool right = input.x < input.width && input.x >= input.width - border_x;
        const bool top = input.y >= 0 && input.y < border_y;
        const bool bottom = input.y < input.height && input.y >= input.height - border_y;

        if (top && left) return FramelessHitTestArea::TopLeft;
        if (top && right) return FramelessHitTestArea::TopRight;
        if (bottom && left) return FramelessHitTestArea::BottomLeft;
        if (bottom && right) return FramelessHitTestArea::BottomRight;
        if (top) return FramelessHitTestArea::Top;
        if (bottom) return FramelessHitTestArea::Bottom;
        if (left) return FramelessHitTestArea::Left;
        if (right) return FramelessHitTestArea::Right;
    }

    if (input.y >= 0 && input.y < drag_height) return FramelessHitTestArea::Caption;
    return FramelessHitTestArea::Client;
}

std::optional<FramelessHitTestArea> parse_resize_direction(std::string_view direction) {
    if (direction == "top") return FramelessHitTestArea::Top;
    if (direction == "bottom") return FramelessHitTestArea::Bottom;
    if (direction == "left") return FramelessHitTestArea::Left;
    if (direction == "right") return FramelessHitTestArea::Right;
    if (direction == "top-left") return FramelessHitTestArea::TopLeft;
    if (direction == "top-right") return FramelessHitTestArea::TopRight;
    if (direction == "bottom-left") return FramelessHitTestArea::BottomLeft;
    if (direction == "bottom-right") return FramelessHitTestArea::BottomRight;
    return std::nullopt;
}

bool windows_needs_self_drawn_top_border(std::uint32_t build) {
    return build >= kWindows10FirstBuild && build < kWindows11FirstBuild;
}

int self_drawn_top_border_thickness(int system_dpi) {
    return std::max(1, system_dpi / 96);
}

int self_drawn_top_border_inset(const SelfDrawnTopBorderLayoutInput& input) {
    if (!input.supported || !input.has_resize_frame) return 0;
    if (input.maximized || input.minimized) return 0;
    return self_drawn_top_border_thickness(input.system_dpi);
}

RgbColor self_drawn_top_border_color(const SelfDrawnTopBorderColorInput& input) {
    if (input.active && input.accent_on_borders && input.colorization_color &&
        input.colorization_balance) {
        std::uint32_t balance = *input.colorization_balance;
        if (balance > 100) balance = 80;
        const std::uint32_t colorization = *input.colorization_color;
        const RgbColor accent{
            static_cast<std::uint8_t>((colorization >> 16) & 0xFF),
            static_cast<std::uint8_t>((colorization >> 8) & 0xFF),
            static_cast<std::uint8_t>(colorization & 0xFF),
        };
        return blend(accent, RgbColor{0xD9, 0xD9, 0xD9}, balance / 100.0);
    }
    if (input.active) {
        if (input.windows_build != 0 &&
            input.windows_build < kWindows10Version1809Build) {
            return RgbColor{0xFF, 0xFF, 0xFF};
        }
        return blend(RgbColor{0x26, 0x26, 0x26}, input.background, 0xA8 / 255.0);
    }
    return blend(RgbColor{0x55, 0x55, 0x55}, input.background, 0x80 / 255.0);
}

} // namespace acecode::desktop