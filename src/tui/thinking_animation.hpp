#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace acecode { namespace tui {

inline constexpr int kThinkingAnimationFrameMs = 60;
inline constexpr int kStreamingRedrawFrameMs = 50;
inline constexpr int kInteractiveBackgroundFrameMs = 250;
inline constexpr int kRecentKeyboardInputWindowMs = 750;
inline constexpr int kMaxAdaptiveAnimationFrameMs = 400;
inline constexpr int kFrameCostBackoffMultiplier = 3;
inline constexpr int kDefaultAnimationFrameMs = 300;
inline constexpr int kConhostAnimationFrameMs = 1000;
inline constexpr int kDragAutoscrollFrameMs = 50;

inline constexpr double kThinkingShimmerCellsPerSecond = 20.25;
inline constexpr double kThinkingShimmerEdgePaddingCells = 2.5;

struct ThinkingGlyphHighlight {
    float warm = 0.0f;
    float white = 0.0f;
};

struct ThinkingAnimationFrame {
    double highlight_center = -kThinkingShimmerEdgePaddingCells;
    std::vector<ThinkingGlyphHighlight> glyph_highlights;
};

struct ThinkingAnimationPacingContext {
    bool conhost_compat_layout = false;
    bool thinking_visible = false;
    bool drag_autoscroll_active = false;
    bool keyboard_input_recent = false;
    int last_frame_latency_ms = 0;
};

// Keyboard activity is recent only for a valid monotonic timestamp inside the
// interaction window. A zero or future timestamp is treated as unavailable.
bool is_keyboard_input_recent(std::int64_t now_ms,
                              std::int64_t last_keyboard_input_ms);

// Select the shared ticker's next wake interval. Drag autoscroll keeps the
// highest priority; conhost keeps its compatibility cadence. Modern thinking
// frames back off for recent typing and expensive completed frames.
int select_animation_frame_interval_ms(
    const ThinkingAnimationPacingContext& context);

// Select the minimum interval for coalescible streamed-delta redraws.
int select_streaming_redraw_interval_ms(bool keyboard_input_recent,
                                        int last_frame_latency_ms);

// Build one deterministic shimmer frame. Each glyph receives bounded warm
// trail and white-core weights for two-stage color interpolation.
ThinkingAnimationFrame make_thinking_animation_frame(std::size_t glyph_count,
                                                      std::int64_t elapsed_ms);

}} // namespace acecode::tui
