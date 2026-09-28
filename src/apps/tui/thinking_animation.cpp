#include "tui/thinking_animation.hpp"

#include <algorithm>
#include <cmath>

namespace acecode { namespace tui {

namespace {

constexpr double kWarmTrailOffsetCells = 1.4;
constexpr double kWarmTrailSigmaCells = 1.1;
constexpr double kWhiteCoreSigmaCells = 0.6;
constexpr double kOffTextFadeSigmaCells = 1.0;

int adaptive_background_interval_ms(int base_interval_ms,
                                    int last_frame_latency_ms) {
    const int bounded_latency_ms = std::clamp(
        last_frame_latency_ms,
        0,
        kMaxAdaptiveAnimationFrameMs);
    const int cost_interval_ms = std::min(
        kMaxAdaptiveAnimationFrameMs,
        bounded_latency_ms * kFrameCostBackoffMultiplier);
    return std::clamp(
        std::max(base_interval_ms, cost_interval_ms),
        base_interval_ms,
        kMaxAdaptiveAnimationFrameMs);
}

double gaussian_weight(double distance, double sigma) {
    const double normalized = distance / sigma;
    return std::exp(-0.5 * normalized * normalized);
}

} // namespace

bool is_keyboard_input_recent(std::int64_t now_ms,
                              std::int64_t last_keyboard_input_ms) {
    if (last_keyboard_input_ms <= 0 || now_ms < last_keyboard_input_ms) {
        return false;
    }
    return now_ms - last_keyboard_input_ms <=
        kRecentKeyboardInputWindowMs;
}

int select_animation_frame_interval_ms(
    const ThinkingAnimationPacingContext& context) {
    if (context.drag_autoscroll_active) return kDragAutoscrollFrameMs;
    if (context.conhost_compat_layout) return kConhostAnimationFrameMs;
    if (context.thinking_visible) {
        const int base_interval_ms = context.keyboard_input_recent
            ? kInteractiveBackgroundFrameMs
            : kThinkingAnimationFrameMs;
        return adaptive_background_interval_ms(
            base_interval_ms, context.last_frame_latency_ms);
    }
    return kDefaultAnimationFrameMs;
}

int select_streaming_redraw_interval_ms(bool keyboard_input_recent,
                                        int last_frame_latency_ms) {
    const int base_interval_ms = keyboard_input_recent
        ? kInteractiveBackgroundFrameMs
        : kStreamingRedrawFrameMs;
    return adaptive_background_interval_ms(
        base_interval_ms, last_frame_latency_ms);
}

ThinkingAnimationFrame make_thinking_animation_frame(std::size_t glyph_count,
                                                      std::int64_t elapsed_ms) {
    ThinkingAnimationFrame frame;
    if (glyph_count == 0) return frame;

    elapsed_ms = std::max<std::int64_t>(0, elapsed_ms);
    const double last_glyph = static_cast<double>(glyph_count - 1);
    const double cycle_cells =
        last_glyph + 2.0 * kThinkingShimmerEdgePaddingCells;
    const double travelled_cells = std::fmod(
        static_cast<double>(elapsed_ms) *
            kThinkingShimmerCellsPerSecond / 1000.0,
        cycle_cells);
    frame.highlight_center =
        travelled_cells - kThinkingShimmerEdgePaddingCells;

    double off_text_distance = 0.0;
    if (frame.highlight_center < 0.0) {
        off_text_distance = -frame.highlight_center;
    } else if (frame.highlight_center > last_glyph) {
        off_text_distance = frame.highlight_center - last_glyph;
    }
    const double edge_visibility =
        gaussian_weight(off_text_distance, kOffTextFadeSigmaCells);

    frame.glyph_highlights.reserve(glyph_count);
    for (std::size_t i = 0; i < glyph_count; ++i) {
        const double glyph_position = static_cast<double>(i);
        const double warm_center =
            frame.highlight_center - kWarmTrailOffsetCells;
        const double warm = edge_visibility * gaussian_weight(
            glyph_position - warm_center, kWarmTrailSigmaCells);
        const double white = edge_visibility * gaussian_weight(
            glyph_position - frame.highlight_center, kWhiteCoreSigmaCells);
        frame.glyph_highlights.push_back({
            static_cast<float>(std::clamp(warm, 0.0, 1.0)),
            static_cast<float>(std::clamp(white, 0.0, 1.0)),
        });
    }
    return frame;
}

}} // namespace acecode::tui
