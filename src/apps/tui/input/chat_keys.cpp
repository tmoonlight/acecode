#include "tui/input/tui_input_context.hpp"
#include "config/config.hpp"
#include "tui/terminal_key_event.hpp"
#include "tui/input/input_trace.hpp"
#include "utils/logger.hpp"
#include "tui/input/chat_keys.hpp"
#include "tui/terminal_key_event.hpp"
#include "tui/picker_scroll.hpp"
#include <algorithm>
using ftxui::Event;
using ftxui::Mouse;
using ftxui::Box;
#include "tui/compact_notice_row.hpp"
#include "tui/tool_row_format.hpp"

namespace acecode::tui {
InputDisposition handle_chat_ctrl_e(TuiState& state, IScreenPort& screen,
    ChatViewport& viewport, const ftxui::Event& event) {
    // Ctrl+E contextual expand: when a summarized tool_result is focused
    // in the chat view, toggle its expanded state. Falls through to the
    // readline-style "move to end of line" when no chat message is focused.
    if (tui::matches_terminal_codepoint(event, 'e', tui::kTerminalCtrl)) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.chat_focus_index >= 0 &&
            state.chat_focus_index < static_cast<int>(state.conversation.size())) {
            auto& msg = state.conversation[state.chat_focus_index];
            const auto tool_result_names =
                acecode::tui::compute_tool_result_names(
                    state.conversation);
            const std::string paired_tool_name =
                state.chat_focus_index <
                    static_cast<int>(tool_result_names.size())
                ? tool_result_names[
                    static_cast<std::size_t>(state.chat_focus_index)]
                : std::string();
            if (acecode::tui::toggle_completed_compact_notice_row(msg)) {
                viewport.invalidate(state.chat_focus_index);
                screen.post_event(Event::Custom);
                return InputDisposition::Consumed;
            }
            if (msg.role == "tool_result" &&
                (msg.summary.has_value() || msg.hunks.has_value()) &&
                !acecode::tui::is_task_complete_result(
                    msg, paired_tool_name)) {
                msg.expanded = !msg.expanded;
                viewport.invalidate(state.chat_focus_index);
                screen.post_event(Event::Custom);
                return InputDisposition::Consumed;
            }
        }
        // Fall through to end-of-line if nothing to toggle.
    }
    return InputDisposition::Continue;
}

InputDisposition handle_chat_page_up(TuiInputContext& context, const Event& event) {
    auto& state = context.state;
    auto& screen = context.screen;
    auto& viewport = context.viewport;
    auto& chat_box = context.viewport.chat_box;
    auto& message_line_counts = context.viewport.message_line_counts;
    auto& message_spacer_rows_after = context.viewport.message_spacer_rows_after;
    auto& config = context.config;
    if (event == Event::PageUp) {
        std::lock_guard<std::mutex> lk(state.mu);
        viewport.sync_from_layout(state);
        // page_keys_single_line 默认开启:把 PgUp 当作单行滚动,等同 Alt+↑.
        // 适用于吞掉 Alt+方向键序列的终端 (老 conhost / Cmder / 部分 SSH 客户端).
        int step = config.tui.page_keys_single_line
            ? 1
            : std::max(1, (chat_box.y_max - chat_box.y_min + 1) - 2);  // 视口高度 -2 留重叠
        const int before_top = state.chat_scroll_top_row;
        const int before_focus = state.chat_focus_index;
        const int before_offset = state.chat_line_offset;
        const bool before_follow_tail = state.chat_follow_tail;
        const int actual = viewport.scroll_by_lines(state, -step);
        ACECODE_INPUT_TRACE(
        const int max_top = acecode::tui::chat_max_scroll_top_row(
            message_line_counts,
            static_cast<int>(state.conversation.size()),
            viewport.rows(), message_spacer_rows_after);
        LOG_DEBUG("[input] chat page up step=" + std::to_string(step) +
                  " actual=" + std::to_string(actual) +
                  " top=" + std::to_string(before_top) + "->" +
                  std::to_string(state.chat_scroll_top_row) +
                  " max_top=" + std::to_string(max_top) +
                  " focus=" + std::to_string(before_focus) + "->" +
                  std::to_string(state.chat_focus_index) +
                  " offset=" + std::to_string(before_offset) + "->" +
                  std::to_string(state.chat_line_offset) +
                  " follow_tail=" +
                  std::string(before_follow_tail ? "1" : "0") + "->" +
                  std::string(state.chat_follow_tail ? "1" : "0"));
        );
        if (actual != 0) {
            screen.post_event(Event::Custom);
        }
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}
InputDisposition handle_chat_page_down(TuiInputContext& context, const Event& event) {
    auto& state = context.state;
    auto& screen = context.screen;
    auto& viewport = context.viewport;
    auto& chat_box = context.viewport.chat_box;
    auto& message_line_counts = context.viewport.message_line_counts;
    auto& message_spacer_rows_after = context.viewport.message_spacer_rows_after;
    auto& config = context.config;
    if (event == Event::PageDown) {
        std::lock_guard<std::mutex> lk(state.mu);
        viewport.sync_from_layout(state);
        int step = config.tui.page_keys_single_line
            ? 1
            : std::max(1, (chat_box.y_max - chat_box.y_min + 1) - 2);
        const int before_top = state.chat_scroll_top_row;
        const int before_focus = state.chat_focus_index;
        const int before_offset = state.chat_line_offset;
        const bool before_follow_tail = state.chat_follow_tail;
        const int actual = viewport.scroll_by_lines(state, step);
        ACECODE_INPUT_TRACE(
        const int max_top = acecode::tui::chat_max_scroll_top_row(
            message_line_counts,
            static_cast<int>(state.conversation.size()),
            viewport.rows(), message_spacer_rows_after);
        LOG_DEBUG("[input] chat page down step=" + std::to_string(step) +
                  " actual=" + std::to_string(actual) +
                  " top=" + std::to_string(before_top) + "->" +
                  std::to_string(state.chat_scroll_top_row) +
                  " max_top=" + std::to_string(max_top) +
                  " focus=" + std::to_string(before_focus) + "->" +
                  std::to_string(state.chat_focus_index) +
                  " offset=" + std::to_string(before_offset) + "->" +
                  std::to_string(state.chat_line_offset) +
                  " follow_tail=" +
                  std::string(before_follow_tail ? "1" : "0") + "->" +
                  std::string(state.chat_follow_tail ? "1" : "0"));
        );
        if (actual != 0) {
            screen.post_event(Event::Custom);
        }
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}
InputDisposition handle_chat_alt_up(TuiInputContext& context, const Event& event) {
    auto& state = context.state;
    auto& screen = context.screen;
    auto& viewport = context.viewport;
    if (tui::matches_terminal_key(
            event, acecode::tui::TerminalKey::ArrowUp, tui::kTerminalAlt)) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.resume_picker_active) return InputDisposition::Consumed;
        if (state.model_picker_open) return InputDisposition::Consumed;
        if (state.mode_picker_open) return InputDisposition::Consumed;
        viewport.sync_from_layout(state);
        if (viewport.scroll_by_lines(state, -1) != 0) {
            screen.post_event(Event::Custom);
        }
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}
InputDisposition handle_chat_alt_down(TuiInputContext& context, const Event& event) {
    auto& state = context.state;
    auto& screen = context.screen;
    auto& viewport = context.viewport;
    if (tui::matches_terminal_key(
            event, acecode::tui::TerminalKey::ArrowDown, tui::kTerminalAlt)) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.resume_picker_active) return InputDisposition::Consumed;
        if (state.model_picker_open) return InputDisposition::Consumed;
        if (state.mode_picker_open) return InputDisposition::Consumed;
        viewport.sync_from_layout(state);
        if (viewport.scroll_by_lines(state, 1) != 0) {
            screen.post_event(Event::Custom);
        }
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}
InputDisposition handle_chat_home(TuiInputContext& context, const Event& event) {
    auto& state = context.state;
    auto& screen = context.screen;
    if (event == Event::Home) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (!state.conversation.empty()) {
            state.chat_scroll_top_row = 0;
            state.chat_focus_index = 0;
            state.chat_line_offset = 0;
            state.chat_follow_tail = false;
            screen.post_event(Event::Custom);
        }
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}
InputDisposition handle_chat_end(TuiInputContext& context, const Event& event) {
    auto& state = context.state;
    auto& screen = context.screen;
    auto& viewport = context.viewport;
    if (event == Event::End) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (!state.conversation.empty()) {
            state.chat_follow_tail = true;
            viewport.clamp_focus(state);
            screen.post_event(Event::Custom);
        }
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}
InputDisposition handle_chat_ctrl_o(TuiInputContext& context, const Event& event) {
    auto& state = context.state;
    auto& screen = context.screen;
    if (tui::matches_terminal_codepoint(event, 'o', tui::kTerminalCtrl)) {
        std::lock_guard<std::mutex> lk(state.mu);
        state.transcript_expanded = !state.transcript_expanded;
        state.sidebar_scroll_top_row = 0;
        state.sidebar_scrollbar_dragging = false;
        state.sidebar_scrollbar_grab_offset_2x = 0;
        screen.post_event(Event::Custom);
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

InputDisposition handle_chat_ctrl_e(TuiInputContext& context, const Event& event) {
    return handle_chat_ctrl_e(context.state, context.screen, context.viewport, event);
}

}
