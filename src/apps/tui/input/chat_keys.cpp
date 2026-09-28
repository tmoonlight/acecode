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

}
