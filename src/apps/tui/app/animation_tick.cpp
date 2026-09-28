#include "tui/app/animation_tick.hpp"
#include "tui/overlays/ask_session_projection.hpp"
#include "tui/ctrl_c_exit.hpp"
#include "tui/thinking_animation.hpp"
#include "tui/model/mcp_sidebar_model.hpp"
#include "tui/input/input_trace.hpp"
#include "utils/logger.hpp"
namespace acecode::tui {
void advance_animation_phase(std::atomic<int>& anim_tick,
    std::chrono::steady_clock::time_point& last_legacy_tick_at,
    std::chrono::steady_clock::time_point tick_now, std::chrono::milliseconds legacy_tick_period) {
    const auto legacy_elapsed = tick_now - last_legacy_tick_at;
    if (legacy_elapsed >= legacy_tick_period) {
        const auto legacy_steps =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                legacy_elapsed).count() /
            legacy_tick_period.count();
        anim_tick.fetch_add(static_cast<int>(legacy_steps));
        last_legacy_tick_at += legacy_tick_period * legacy_steps;
    }
}
AnimationTickResult animation_tick_locked(TuiState& state, ChatViewport& viewport,
    std::chrono::steady_clock::time_point now) {
    bool requires_immediate_post = false;
    bool background_animation_visible = false;
    bool should_post = false;
    if (state.ask_session) {
        const auto ask_effects = state.ask_session->tick(now);
        if (!ask_effects.empty()) {
            tui::dispatch_ask_session_effects_locked(state, ask_effects);
            requires_immediate_post = true;
        }
    }
    if (state.status_line_clear_at.time_since_epoch().count() != 0 &&
        now >= state.status_line_clear_at) {
        state.status_line = state.status_line_saved;
        state.status_line_saved.clear();
        state.status_line_clear_at = {};
        requires_immediate_post = true;
    }
    if (acecode::tui::expire_ctrl_c_exit_state(
            state.ctrl_c_armed, state.last_ctrl_c_time, now)) {
        requires_immediate_post = true;
    }
    // link-hover-tooltip (add-tui-hyperlinks 5.3): 指针在链接上
    // 无按键停留 >= 300ms 后显示气泡。悬停状态由事件线程在
    // Mouse::Moved 时更新,这里只做时间门 —— 到期置位并强制
    // 下一帧渲染,气泡出现不依赖任何后续输入事件。
    if (!state.hover_link_href.empty() &&
        !state.hover_link_visible) {
        const auto hover_elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - state.hover_link_since).count();
        if (hover_elapsed_ms >=
            acecode::tui::kLinkHoverTooltipDelayMs) {
            state.hover_link_visible = true;
            requires_immediate_post = true;
        }
    }
    background_animation_visible = tui::mcp_sidebar_has_loading(state);

    // drag-autoscroll: 时间门到点就滚一行, 把 ShiftSelection 的补偿请求
    // 累加到 pending_shift_dy, 由事件线程 CatchEvent 的入口消费 — 避免
    // anim_thread 直接改 FTXUI selection_data_ 与 HandleSelection 撞车.
    if (state.drag_phase == drag_scroll::Phase::ScrollingUp ||
        state.drag_phase == drag_scroll::Phase::ScrollingDown) {
        if (drag_scroll::should_tick(now, state.last_drag_scroll_at,
                                      std::chrono::milliseconds(60))) {
            int dy = (state.drag_phase == drag_scroll::Phase::ScrollingUp) ? -1 : 1;
            int actual = viewport.scroll_by_lines(state, dy);
            if (actual != 0) {
                // viewport.scroll_by_lines(state, +1) 让视口顶部下移一行 → 屏幕
                // 内容相对上移 actual 行 → 原 anchor 文本屏幕坐标应
                // -actual.
                state.pending_shift_dy += -actual;
                ACECODE_INPUT_TRACE(
                LOG_DEBUG("[drag-select] autoscroll tick phase=" +
                          tui::drag_phase_for_log(state.drag_phase) +
                          " requested_dy=" + std::to_string(dy) +
                          " actual=" + std::to_string(actual) +
                          " pending_shift_dy=" +
                          std::to_string(state.pending_shift_dy) +
                          " focus=" +
                          std::to_string(state.chat_focus_index) +
                          " offset=" +
                          std::to_string(state.chat_line_offset) +
                          " mouse=(" +
                          std::to_string(state.last_mouse_x) +
                          "," +
                          std::to_string(state.last_mouse_y) +
                          ")");
                );
                requires_immediate_post = true;
            }
        }
    }

    // Drive re-render while waiting on the LLM, while live tool/MCP
    // progress is visible, or when status cleanup reaches its deadline.
    // Subagent sidebar snapshots publish their own render event on each
    // lifecycle update and no longer contain a ticking elapsed readout.
    // 把读取留在同一把锁里,避免 ticker 与回调并发改 busy 字段。
    should_post = requires_immediate_post ||
        background_animation_visible ||
        state.is_waiting ||
        state.tool_running;
    return {requires_immediate_post, should_post};
}
}
