#include <gtest/gtest.h>
#include "tui/app/animation_tick.hpp"
#include "tui/ctrl_c_exit.hpp"
namespace {
using Clock = std::chrono::steady_clock;
Clock::time_point at_ms(long value) { return Clock::time_point(std::chrono::milliseconds(value)); }
}
// 中文时间契约：真实已过时间累加旧动画 phase，短间隔不能加速 MCP 动画，也不丢余数。
TEST(AnimationTick, LegacyPhaseCountsElapsedPeriodsAndKeepsRemainder) {
    std::atomic<int> phase{7};
    auto previous = at_ms(1000);
    acecode::tui::advance_animation_phase(phase, previous, at_ms(1299), std::chrono::milliseconds(300));
    EXPECT_EQ(phase.load(), 7);
    acecode::tui::advance_animation_phase(phase, previous, at_ms(1950), std::chrono::milliseconds(300));
    EXPECT_EQ(phase.load(), 10);
    EXPECT_EQ(previous, at_ms(1900));
}
// 中文回归说明：状态提示和 Ctrl+C 到期即请求最后一帧，即使没有活动动画。
TEST(AnimationTick, ExpiryRestoresStatusAndRequestsImmediateRedraw) {
    acecode::TuiState state;
    acecode::tui::ChatViewport viewport;
    std::lock_guard<std::mutex> lock(state.mu);
    state.status_line = "Copied";
    state.status_line_saved = "normal";
    state.status_line_clear_at = at_ms(3000);
    state.ctrl_c_armed = true;
    state.last_ctrl_c_time = at_ms(2000);
    auto result = acecode::tui::animation_tick_locked(state, viewport, at_ms(2999));
    EXPECT_FALSE(result.immediate);
    result = acecode::tui::animation_tick_locked(state, viewport, at_ms(3001));
    EXPECT_TRUE(result.immediate);
    EXPECT_TRUE(result.should_post);
    EXPECT_EQ(state.status_line, "normal");
    EXPECT_TRUE(state.status_line_saved.empty());
    EXPECT_EQ(state.status_line_clear_at, Clock::time_point{});
    EXPECT_FALSE(state.ctrl_c_armed);
    EXPECT_FALSE(acecode::tui::animation_tick_locked(state, viewport, at_ms(4000)).should_post);
}
// 中文边界说明：悬停必须达到 300ms 才显示；显示后不因静止指针反复请求即时帧。
TEST(AnimationTick, HoverUsesOriginalDeadline) {
    acecode::TuiState state;
    acecode::tui::ChatViewport viewport;
    std::lock_guard<std::mutex> lock(state.mu);
    state.hover_link_href = "https://example.test";
    state.hover_link_since = at_ms(1000);
    EXPECT_FALSE(acecode::tui::animation_tick_locked(state, viewport, at_ms(1299)).immediate);
    EXPECT_FALSE(state.hover_link_visible);
    EXPECT_TRUE(acecode::tui::animation_tick_locked(state, viewport, at_ms(1300)).immediate);
    EXPECT_TRUE(state.hover_link_visible);
    EXPECT_FALSE(acecode::tui::animation_tick_locked(state, viewport, at_ms(1600)).immediate);
}
// 中文事件所有权说明：tick 仅累积选区偏移，实际 ShiftSelection 仍由输入线程执行。
TEST(AnimationTick, DragScrollQueuesSelectionCompensationAndHonorsTimeGate) {
    acecode::TuiState state;
    acecode::tui::ChatViewport viewport;
    std::lock_guard<std::mutex> lock(state.mu);
    viewport.chat_box = {0, 79, 0, 4};
    viewport.message_line_counts = {30};
    viewport.message_spacer_rows_after = {0};
    state.conversation.push_back({"assistant", "long message", false});
    state.chat_follow_tail = false;
    state.chat_scroll_top_row = 5;
    state.drag_phase = acecode::drag_scroll::Phase::ScrollingDown;
    state.last_drag_scroll_at = at_ms(1000);
    EXPECT_FALSE(acecode::tui::animation_tick_locked(state, viewport, at_ms(1059)).immediate);
    EXPECT_EQ(state.pending_shift_dy, 0);
    EXPECT_TRUE(acecode::tui::animation_tick_locked(state, viewport, at_ms(1060)).immediate);
    EXPECT_EQ(state.chat_scroll_top_row, 6);
    EXPECT_EQ(state.pending_shift_dy, -1);
    EXPECT_EQ(state.last_drag_scroll_at, at_ms(1060));
}
// 中文回归说明：静止空闲不重绘，等待和工具状态使用合并帧而非无条件即时帧。
TEST(AnimationTick, BusyAndToolProgressUseScheduledRedraw) {
    acecode::TuiState state;
    acecode::tui::ChatViewport viewport;
    std::lock_guard<std::mutex> lock(state.mu);
    EXPECT_FALSE(acecode::tui::animation_tick_locked(state, viewport, at_ms(1000)).should_post);
    state.is_waiting = true;
    auto result = acecode::tui::animation_tick_locked(state, viewport, at_ms(1000));
    EXPECT_FALSE(result.immediate);
    EXPECT_TRUE(result.should_post);
    state.is_waiting = false;
    state.tool_running = true;
    EXPECT_TRUE(acecode::tui::animation_tick_locked(state, viewport, at_ms(1000)).should_post);
}
