#include "custom_toast.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

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

ToastChromeGeometry compute_toast_chrome_geometry(int surface_x,
                                                  int surface_y,
                                                  int surface_width,
                                                  int surface_height,
                                                  int chrome_inset) {
    const int inset = std::max(0, chrome_inset);
    const int width = std::max(0, surface_width);
    const int height = std::max(0, surface_height);
    return {
        surface_x - inset,
        surface_y - inset,
        width + inset * 2,
        height + inset * 2,
        inset,
        inset,
        width,
        height,
    };
}

double toast_rounded_rect_distance(double x,
                                   double y,
                                   int width,
                                   int height,
                                   int radius) {
    if (width <= 0 || height <= 0) {
        return std::numeric_limits<double>::infinity();
    }
    const double half_width = static_cast<double>(width) / 2.0;
    const double half_height = static_cast<double>(height) / 2.0;
    const double clamped_radius = std::clamp(
        static_cast<double>(std::max(0, radius)),
        0.0,
        std::min(half_width, half_height));
    const double qx =
        std::abs(x - half_width) - (half_width - clamped_radius);
    const double qy =
        std::abs(y - half_height) - (half_height - clamped_radius);
    const double outside = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0));
    const double inside = std::min(std::max(qx, qy), 0.0);
    return outside + inside - clamped_radius;
}

std::uint8_t toast_surface_coverage(double signed_distance) {
    const double coverage = std::clamp(0.5 - signed_distance, 0.0, 1.0);
    return static_cast<std::uint8_t>(std::lround(coverage * 255.0));
}

std::uint8_t toast_shadow_alpha(double signed_distance,
                                int blur,
                                int max_alpha) {
    if (blur <= 0 || max_alpha <= 0 ||
        signed_distance >= static_cast<double>(blur)) {
        return 0;
    }
    const double normalized =
        1.0 - std::max(0.0, signed_distance) / static_cast<double>(blur);
    const int clamped_max = std::min(255, max_alpha);
    return static_cast<std::uint8_t>(std::lround(
        static_cast<double>(clamped_max) * normalized * normalized));
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
