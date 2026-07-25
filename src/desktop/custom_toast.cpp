#include "custom_toast.hpp"

#include <algorithm>
#include <cmath>

namespace acecode::desktop::custom_toast {
namespace {

float clamp_unit(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

float normalized_time(std::uint32_t elapsed_ms, std::uint32_t duration_ms) {
    if (duration_ms == 0) return 1.0f;
    if (elapsed_ms >= duration_ms) return 1.0f;
    return static_cast<float>(elapsed_ms) / static_cast<float>(duration_ms);
}

// Decelerating exponential ease normalized to hit exactly 0 and 1 at the ends.
float exponential_ease(float time) {
    constexpr float a = -8.0f;
    return (std::exp(a * time) - 1.0f) / (std::exp(a) - 1.0f);
}

} // namespace

int scale_for_dpi(int value, unsigned dpi) {
    if (dpi == 0) dpi = kBaseDpi;
    const long long scaled =
        static_cast<long long>(value) * static_cast<long long>(dpi) /
        static_cast<long long>(kBaseDpi);
    return static_cast<int>(scaled);
}

float ease_in_position(std::uint32_t elapsed_ms, std::uint32_t duration_ms) {
    return clamp_unit(exponential_ease(normalized_time(elapsed_ms, duration_ms)));
}

float ease_out_position(std::uint32_t elapsed_ms, std::uint32_t duration_ms) {
    const float time = normalized_time(elapsed_ms, duration_ms);
    return clamp_unit(1.0f - std::sqrt(1.0f - time * time));
}

float stack_collapse_position(std::uint32_t elapsed_ms,
                              std::uint32_t duration_ms) {
    return clamp_unit(exponential_ease(normalized_time(elapsed_ms, duration_ms)));
}

unsigned clamp_auto_dismiss_seconds(unsigned raw_seconds) {
    constexpr unsigned kMin = 6;
    constexpr unsigned kMax = 30;
    if (raw_seconds < kMin) return kMin;
    if (raw_seconds > kMax) return kMax;
    return raw_seconds;
}

ToastPlacement compute_toast_placement(const ToastRect& work_area,
                                       int margin_x,
                                       int margin_y,
                                       int width,
                                       int height,
                                       int vertical_offset,
                                       float ease_in) {
    ToastPlacement placement;
    const float progress = clamp_unit(ease_in);
    placement.width =
        static_cast<int>(std::lround(static_cast<double>(width) * progress));
    placement.width = std::max(0, std::min(width, placement.width));
    placement.height = height;
    // The right edge stays pinned while the width grows, so the card appears
    // to slide out of the screen edge instead of scaling from its center.
    placement.x = work_area.right - margin_x - placement.width;
    placement.y = work_area.bottom - margin_y - vertical_offset - height;
    return placement;
}

std::vector<int> compute_stack_offsets(const std::vector<int>& heights,
                                       int margin) {
    std::vector<int> offsets;
    offsets.reserve(heights.size());
    int cursor = 0;
    for (int height : heights) {
        offsets.push_back(cursor);
        cursor += height + margin;
    }
    return offsets;
}

int fit_body_lines(int available_height, int line_height, int max_lines) {
    if (line_height <= 0 || max_lines <= 0) return 0;
    if (available_height < line_height) return 0;
    const int fits = available_height / line_height;
    return std::min(fits, max_lines);
}

} // namespace acecode::desktop::custom_toast

#ifndef _WIN32

namespace acecode::desktop::custom_toast {

bool initialize(const InitOptions& /*options*/) {
    return false;
}

bool is_available() {
    return false;
}

bool show(const NotifyPayload& /*payload*/) {
    return false;
}

void shutdown() {}

} // namespace acecode::desktop::custom_toast

#endif // !_WIN32
