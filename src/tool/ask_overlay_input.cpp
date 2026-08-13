#include "ask_overlay_input.hpp"

#include "../tui_state.hpp"
#include "../tui/tui_helpers.hpp"
#include "../tui/paste_handler.hpp"
#include "../tui/terminal_key_event.hpp"
#include "../utils/text_input_ops.hpp"

#include <string>

namespace acecode {

namespace {

// Home 等价按键集合:FTXUI 的 Event::Home + 几个 VT 系列 / rxvt 系列的
// ESC-sequence 原始字节。Ctrl+A 现在改为全选当前 buffer。
bool is_home_event(const ftxui::Event& e) {
    return acecode::tui::matches_terminal_key(
        e, acecode::tui::TerminalKey::Home);
}

bool is_end_event(const ftxui::Event& e) {
    const auto ctrl = acecode::tui::terminal_modifier(
        acecode::tui::TerminalKeyModifier::Ctrl);
    return acecode::tui::matches_terminal_key(
               e, acecode::tui::TerminalKey::End) ||
           acecode::tui::matches_terminal_codepoint(e, 'e', ctrl);
}

} // namespace

bool try_handle_ask_other_input(TuiState& state, const ftxui::Event& event) {
    // 可打印字符 —— 内联处理,不触发 slash-dropdown refresh / shell-mode
    // trigger / history_index 重置等所有无关副作用。
    if (event.is_character()) {
        insert_replacing_selection(
            state.input_text,
            state.input_cursor,
            state.input_selection_anchor,
            event.character());
        acecode::tui::prune_unreferenced(
            state.pasted_texts, state.input_text);
        state.input_vertical_goal_column.reset();
        return true;
    }

    if (event == ftxui::Event::Backspace) {
        if (!erase_text_selection(
                state.input_text,
                state.input_cursor,
                state.input_selection_anchor)) {
            if (auto span = acecode::tui::placeholder_ending_at(
                    state.input_text,
                    state.pasted_texts,
                    state.input_cursor)) {
                state.pasted_texts.erase(span->paste_id);
                state.input_text.erase(
                    span->begin, span->end - span->begin);
                state.input_cursor = span->begin;
            } else {
                backspace_utf8(state.input_text, state.input_cursor);
            }
        }
        acecode::tui::prune_unreferenced(
            state.pasted_texts, state.input_text);
        state.input_vertical_goal_column.reset();
        return true;
    }

    if (event == ftxui::Event::Delete) {
        if (!erase_text_selection(
                state.input_text,
                state.input_cursor,
                state.input_selection_anchor)) {
            if (auto span = acecode::tui::placeholder_starting_at(
                    state.input_text,
                    state.pasted_texts,
                    state.input_cursor)) {
                state.pasted_texts.erase(span->paste_id);
                state.input_text.erase(
                    span->begin, span->end - span->begin);
                state.input_cursor = span->begin;
            } else {
                delete_utf8(state.input_text, state.input_cursor);
            }
        }
        acecode::tui::prune_unreferenced(
            state.pasted_texts, state.input_text);
        state.input_vertical_goal_column.reset();
        return true;
    }

    const auto shifted = acecode::tui::shift_arrow_direction(event);
    if (shifted == acecode::tui::ShiftArrowDirection::Left ||
        shifted == acecode::tui::ShiftArrowDirection::Right) {
        std::size_t target = clamp_utf8_boundary(
            state.input_text, state.input_cursor);
        if (*shifted == acecode::tui::ShiftArrowDirection::Left) {
            if (auto span = acecode::tui::placeholder_ending_at(
                    state.input_text, state.pasted_texts, target)) {
                target = span->begin;
            } else {
                move_cursor_left_utf8(state.input_text, target);
            }
        } else if (auto span = acecode::tui::placeholder_starting_at(
                       state.input_text, state.pasted_texts, target)) {
            target = span->end;
        } else {
            move_cursor_right_utf8(state.input_text, target);
        }
        move_cursor_with_selection(
            state.input_text,
            state.input_cursor,
            state.input_selection_anchor,
            target,
            true);
        state.input_vertical_goal_column.reset();
        return true;
    }

    if (event == ftxui::Event::ArrowLeft) {
        state.input_vertical_goal_column.reset();
        if (collapse_selection_left(
                state.input_text,
                state.input_cursor,
                state.input_selection_anchor)) {
            return true;
        }
        if (auto span = acecode::tui::placeholder_ending_at(
                state.input_text,
                state.pasted_texts,
                state.input_cursor)) {
            state.input_cursor = span->begin;
        } else {
            move_cursor_left_utf8(state.input_text, state.input_cursor);
        }
        return true;
    }

    if (event == ftxui::Event::ArrowRight) {
        state.input_vertical_goal_column.reset();
        if (collapse_selection_right(
                state.input_text,
                state.input_cursor,
                state.input_selection_anchor)) {
            return true;
        }
        if (auto span = acecode::tui::placeholder_starting_at(
                state.input_text,
                state.pasted_texts,
                state.input_cursor)) {
            state.input_cursor = span->end;
        } else {
            move_cursor_right_utf8(state.input_text, state.input_cursor);
        }
        return true;
    }

    if (acecode::tui::matches_terminal_codepoint(
            event,
            'a',
            acecode::tui::terminal_modifier(
                acecode::tui::TerminalKeyModifier::Ctrl))) {
        select_all_text(
            state.input_text,
            state.input_cursor,
            state.input_selection_anchor);
        state.input_vertical_goal_column.reset();
        return true;
    }

    if (is_home_event(event)) {
        state.input_cursor = 0;
        state.input_selection_anchor.reset();
        state.input_vertical_goal_column.reset();
        return true;
    }

    if (is_end_event(event)) {
        state.input_cursor = state.input_text.size();
        state.input_selection_anchor.reset();
        state.input_vertical_goal_column.reset();
        return true;
    }

    // 未识别按键(例如 Event::Custom / Event::Mouse / F1 等):helper 不
    // 改 state,返回 false 告诉调用方 **不要** PostEvent。调用方仍应该
    // 吞掉事件(给 FTXUI 返回 true),防止未识别键透传到下游 handler,
    // 但 PostEvent 仅当 state 真的变了才触发 —— 否则 Custom 事件自回环
    // 会把事件循环卡死。
    return false;
}

} // namespace acecode
