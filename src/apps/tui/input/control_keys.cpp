#include "tui/input/control_keys.hpp"
#include "tui/terminal_key_event.hpp"
#include "tui/input/input_trace.hpp"
#include "utils/logger.hpp"
#include "tui/ctrl_c_exit.hpp"
#include "tui/model/input_state.hpp"
#include "tui/model/status_line.hpp"
#include "tui/composer/suggestions.hpp"
#include "tui/overlays/ask_session_projection.hpp"
#include "tui/overlays/list_picker_input.hpp"
#include "tui/input_history_navigation.hpp"
#include "tui/drag_scroll.hpp"
#include "permissions/permissions.hpp"
#include "session/session_manager.hpp"

using ftxui::Event;
using ftxui::Mouse;
using ftxui::Box;

namespace acecode::tui {
InputDisposition handle_ctrl_c(TuiInputContext& context, const Event& event) {
    auto& state = context.state;
    auto& screen = context.screen;
    auto& viewport = context.viewport;
    auto& cmd_registry = context.commands;
    if (tui::matches_terminal_codepoint(event, 'c', tui::kTerminalCtrl)) {
        // If an operation is active, Ctrl+C behaves like Escape: cancel the
        // current work instead of arming the double-press exit shortcut.
        bool should_exit = false;
        {
            std::lock_guard<std::mutex> lk(state.mu);
            if (state.is_compacting) {
                tui::cancel_ctrl_c_exit_locked(state);
                state.compact_abort_requested.store(true);
                state.conversation.push_back({"system", "Cancelling compaction...", false});
                state.chat_follow_tail = true;
                viewport.clamp_focus(state);
                screen.post_event(Event::Custom);
                return InputDisposition::Consumed;
            }
            if (state.ask_pending || state.confirm_pending ||
                state.is_waiting || state.tool_running) {
                tui::cancel_ctrl_c_exit_locked(state);
                if (state.ask_pending && state.ask_session) {
                    const auto ask_effects = state.ask_session->dispatch(
                        {tui::AskQuestionEventKind::GlobalCancel});
                    tui::dispatch_ask_session_effects_locked(state, ask_effects);
                    screen.post_event(Event::Custom);
                } else {
                    screen.post_event(Event::Escape);
                }
                return InputDisposition::Consumed;
            }
            const auto action = acecode::tui::record_ctrl_c_exit_press(
                state.ctrl_c_armed,
                state.last_ctrl_c_time,
                std::chrono::steady_clock::now());
            if (action == acecode::tui::CtrlCExitAction::Exit) {
                should_exit = true;
            } else {
                if (acecode::tui::clear_current_input_for_history_restore(state)) {
                    tui::refresh_input_suggestions(state, cmd_registry, context.turn.cwd());
                }
            }
        }
        if (should_exit) {
            screen.exit();
        } else {
            screen.post_event(Event::Custom);
        }
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}
InputDisposition handle_escape(TuiInputContext& context, const Event& event) {
    auto& state = context.state;
    auto& screen = context.screen;
    auto& viewport = context.viewport;
    if (tui::matches_terminal_key(event, acecode::tui::TerminalKey::Escape)) {
        std::lock_guard<std::mutex> lk(state.mu);
        // link-hover-tooltip (add-tui-hyperlinks 5.3): Esc 隐藏悬停气泡,
        // 与 "移开指针隐藏" 语义一致。
        if (!state.hover_link_href.empty() || state.hover_link_visible) {
            state.hover_link_href.clear();
            state.hover_link_visible = false;
            screen.post_event(Event::Custom);
        }
        // drag-autoscroll: Esc 中止任何进行中的拖动自动滚动. 选区本身由
        // 下游 FTXUI 通过 `handled=true` 情况下的 HandleSelection 清空
        // (如果之前有选择). 我们只负责把自己的状态机拉回 Idle.
        if (state.drag_left_pressed ||
            state.drag_phase != drag_scroll::Phase::Idle) {
            state.drag_left_pressed = false;
            state.drag_phase = drag_scroll::Phase::Idle;
            state.last_drag_scroll_at = {};
        }
        if (auto result = tui::list_picker_escape_locked(state, screen, viewport);
                result != InputDisposition::Continue) return result;

        // confirm_pending 的 Esc → Deny 已由上面的 confirm overlay handler
        // 拦截,这里不再重复处理。

        // Shell mode: Escape exits and clears the buffer. Takes precedence
        // over `is_waiting` abort so typing Esc in shell mode never fires
        // an unintended cancel on a pending agent turn.
        if (state.input_mode == InputMode::Shell) {
            tui::cancel_ctrl_c_exit_locked(state);
            state.input_mode = InputMode::Normal;
            state.input_text.clear(); state.pasted_texts.clear();
            state.input_cursor = 0;
            state.clear_input_selection();
            return InputDisposition::Consumed;
        }
        if (!state.pending_attachments.empty() && state.input_text.empty()) {
            tui::cancel_ctrl_c_exit_locked(state);
            state.pending_attachments.clear();
            state.pending_attachment_focus =
                acecode::tui::kNoPendingAttachmentFocus;
            tui::set_transient_status_line_locked(state, "Cleared pending attachments");
            screen.post_event(Event::Custom);
            return InputDisposition::Consumed;
        }
        if (state.is_waiting || state.tool_running) {
            // 用户主动中断的回合不追加 "Done for Ns" 行(标记由
            // on_busy_changed(false) 消费复位)。Ctrl+C 在 busy 时
            // PostEvent(Event::Escape) 复用本分支,天然覆盖。
            state.turn_interrupted_by_user = true;
            context.turn.cancel();
            return InputDisposition::Consumed;
        }
    }
    return InputDisposition::Continue;
}
InputDisposition handle_tab(TuiInputContext& context, const Event& event) {
    auto& state = context.state;
    if (event == Event::Tab) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.mode_picker_open) return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}
InputDisposition handle_shift_tab(TuiInputContext& context, const Event& event) {
    auto& state = context.state;
    auto& screen = context.screen;
    auto& viewport = context.viewport;
    auto& permissions = context.permissions;
    auto& session_manager = context.session;
    if (tui::matches_terminal_key(
            event, acecode::tui::TerminalKey::Tab, tui::kTerminalShift)) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.mode_picker_open) return InputDisposition::Consumed;
        if (!state.is_waiting && !state.confirm_pending) {
            const PermissionMode before = permissions.mode();
            auto new_mode = permissions.cycle_mode();
            session_manager.set_permission_mode(PermissionManager::mode_name(new_mode));
            if (new_mode == PermissionMode::Plan) {
                session_manager.set_pre_plan_permission_mode(
                    PermissionManager::mode_name(
                        before == PermissionMode::Plan
                            ? permissions.pre_plan_mode()
                            : before));
                session_manager.ensure_plan_file_path();
            }
            state.conversation.push_back({"system",
                std::string("Permission mode: ") + PermissionManager::mode_name(new_mode) +
                " - " + PermissionManager::mode_description(new_mode), false});
            viewport.clamp_focus(state);
            screen.post_event(Event::Custom);
        }
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

}
