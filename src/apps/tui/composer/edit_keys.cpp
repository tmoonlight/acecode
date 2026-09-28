#include "tui/composer/edit_keys.hpp"
#include "tui/composer/suggestions.hpp"
#include "tui/overlays/list_picker_input.hpp"
#include "tui/text_input_ops.hpp"
#include "tui/paste_handler.hpp"
#include "tui/input_history_navigation.hpp"
#include "tui/pending_attachment_selection.hpp"
#include "tui/terminal_key_event.hpp"
#include <algorithm>
using ftxui::Event;

namespace acecode::tui {
static bool is_home_event(const ftxui::Event& e) {
    return matches_terminal_key(e, TerminalKey::Home);
}
static bool is_end_event(const ftxui::Event& e) {
    return matches_terminal_key(e, TerminalKey::End) || matches_terminal_codepoint(e, 'e', kTerminalCtrl);
}

InputDisposition handle_composer_shift_arrow(TuiInputContext& ctx, const ftxui::Event& event) {
    auto& state = ctx.state;
    auto& screen = ctx.screen;
    auto& input_hit_layout = ctx.geometry.input_hit_layout;
    if (const auto shifted =
            acecode::tui::shift_arrow_direction(event)) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.resume_picker_active || state.model_picker_open ||
            state.mode_picker_open || state.rewind_picker_active ||
            state.confirm_pending) {
            return InputDisposition::Consumed;
        }

        std::optional<size_t> target;
        if (*shifted == acecode::tui::ShiftArrowDirection::Up ||
            *shifted == acecode::tui::ShiftArrowDirection::Down) {
            if (input_hit_layout.input_value == state.input_text) {
                target = acecode::tui::input_cursor_vertical_target(
                    state.input_text,
                    input_hit_layout.box,
                    input_hit_layout.regions,
                    state.input_cursor,
                    *shifted,
                    &state.input_vertical_goal_column);
            }
        } else {
            size_t next = acecode::clamp_utf8_boundary(
                state.input_text, state.input_cursor);
            if (*shifted == acecode::tui::ShiftArrowDirection::Left) {
                if (auto span = acecode::tui::placeholder_ending_at(
                        state.input_text, state.pasted_texts, next)) {
                    next = span->begin;
                } else {
                    acecode::move_cursor_left_utf8(
                        state.input_text, next);
                }
            } else if (auto span =
                           acecode::tui::placeholder_starting_at(
                               state.input_text,
                               state.pasted_texts,
                               next)) {
                next = span->end;
            } else {
                acecode::move_cursor_right_utf8(
                    state.input_text, next);
            }
            state.input_vertical_goal_column.reset();
            target = next;
        }

        if (target.has_value()) {
            acecode::move_cursor_with_selection(
                state.input_text,
                state.input_cursor,
                state.input_selection_anchor,
                *target,
                true);
            state.pending_attachment_focus =
                acecode::tui::kNoPendingAttachmentFocus;
        }
        screen.post_event(Event::Custom);
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

InputDisposition handle_composer_up(TuiInputContext& ctx, const ftxui::Event& event) {
    auto& state = ctx.state;
    auto& screen = ctx.screen;
    auto& cmd_registry = ctx.commands;
    if (event == Event::ArrowUp) {
        std::lock_guard<std::mutex> lk(state.mu);
        state.input_vertical_goal_column.reset();
        if (auto result = tui::input_result(tui::list_picker_up_locked(state, screen))) return input_stopped(*result);

        if (acecode::tui::navigate_input_history_up(state)) {
            // 历史覆盖输入：清掉本会话旧粘贴留下的孤儿 pasted_texts（spec 3.7）。
            acecode::tui::prune_unreferenced(state.pasted_texts, state.input_text);
            tui::refresh_input_suggestions(state, cmd_registry, ctx.turn.cwd());
        }
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

InputDisposition handle_composer_down(TuiInputContext& ctx, const ftxui::Event& event) {
    auto& state = ctx.state;
    auto& screen = ctx.screen;
    auto& cmd_registry = ctx.commands;
    if (event == Event::ArrowDown) {
        std::lock_guard<std::mutex> lk(state.mu);
        state.input_vertical_goal_column.reset();
        if (auto result = tui::input_result(tui::list_picker_down_locked(state, screen))) return input_stopped(*result);

        if (acecode::tui::navigate_input_history_down(state)) {
            // 历史覆盖输入：清掉本会话旧粘贴留下的孤儿 pasted_texts（spec 3.7）。
            acecode::tui::prune_unreferenced(state.pasted_texts, state.input_text);
            tui::refresh_input_suggestions(state, cmd_registry, ctx.turn.cwd());
        }
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

InputDisposition handle_composer_left(TuiInputContext& ctx, const ftxui::Event& event) {
    auto& state = ctx.state;
    if (event == Event::ArrowLeft) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.resume_picker_active) return InputDisposition::Consumed;
        if (state.model_picker_open) return InputDisposition::Consumed;
        if (state.mode_picker_open) return InputDisposition::Consumed;
        state.input_vertical_goal_column.reset();
        if (acecode::collapse_selection_left(
                state.input_text,
                state.input_cursor,
                state.input_selection_anchor)) {
            return InputDisposition::Consumed;
        }
        state.input_cursor = acecode::clamp_utf8_boundary(
            state.input_text, state.input_cursor);
        if (state.input_cursor == 0) return InputDisposition::Consumed;
        // 已知 [Pasted text #N] 占位符整体跨越（atomic span）。
        if (auto span = acecode::tui::placeholder_ending_at(
                state.input_text, state.pasted_texts, state.input_cursor)) {
            state.input_cursor = span->begin;
            return InputDisposition::Consumed;
        }
        acecode::move_cursor_left_utf8(
            state.input_text, state.input_cursor);
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

InputDisposition handle_composer_right(TuiInputContext& ctx, const ftxui::Event& event) {
    auto& state = ctx.state;
    if (event == Event::ArrowRight) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.resume_picker_active) return InputDisposition::Consumed;
        if (state.model_picker_open) return InputDisposition::Consumed;
        if (state.mode_picker_open) return InputDisposition::Consumed;
        state.input_vertical_goal_column.reset();
        if (acecode::collapse_selection_right(
                state.input_text,
                state.input_cursor,
                state.input_selection_anchor)) {
            return InputDisposition::Consumed;
        }
        state.input_cursor = acecode::clamp_utf8_boundary(
            state.input_text, state.input_cursor);
        if (state.input_cursor >= state.input_text.size()) return InputDisposition::Consumed;
        // 已知 [Pasted text #N] 占位符整体跨越（atomic span）。
        if (auto span = acecode::tui::placeholder_starting_at(
                state.input_text, state.pasted_texts, state.input_cursor)) {
            state.input_cursor = span->end;
            return InputDisposition::Consumed;
        }
        acecode::move_cursor_right_utf8(
            state.input_text, state.input_cursor);
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

InputDisposition handle_composer_ctrl_a(TuiInputContext& ctx, const ftxui::Event& event) {
    auto& state = ctx.state;
    auto& screen = ctx.screen;
    if (tui::matches_terminal_codepoint(event, 'a', tui::kTerminalCtrl)) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.resume_picker_active || state.model_picker_open ||
            state.mode_picker_open) {
            return InputDisposition::Consumed;
        }
        acecode::select_all_text(
            state.input_text,
            state.input_cursor,
            state.input_selection_anchor);
        state.input_vertical_goal_column.reset();
        state.pending_attachment_focus =
            acecode::tui::kNoPendingAttachmentFocus;
        screen.post_event(Event::Custom);
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

InputDisposition handle_composer_home(TuiInputContext& ctx, const ftxui::Event& event) {
    auto& state = ctx.state;
    if (is_home_event(event)) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.resume_picker_active) return InputDisposition::Consumed;
        state.input_cursor = 0;
        state.input_selection_anchor.reset();
        state.input_vertical_goal_column.reset();
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

InputDisposition handle_composer_end(TuiInputContext& ctx, const ftxui::Event& event) {
    auto& state = ctx.state;
    if (is_end_event(event)) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.resume_picker_active) return InputDisposition::Consumed;
        state.input_cursor = state.input_text.size();
        state.input_selection_anchor.reset();
        state.input_vertical_goal_column.reset();
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

InputDisposition handle_composer_delete(TuiInputContext& ctx, const ftxui::Event& event) {
    auto& state = ctx.state;
    auto& cmd_registry = ctx.commands;
    if (event == Event::Delete) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.mode_picker_open) return InputDisposition::Consumed;
        state.input_vertical_goal_column.reset();
        if (acecode::erase_text_selection(
                state.input_text,
                state.input_cursor,
                state.input_selection_anchor)) {
            acecode::tui::prune_unreferenced(
                state.pasted_texts, state.input_text);
            tui::refresh_input_suggestions(
                state, cmd_registry, ctx.turn.cwd());
            return InputDisposition::Consumed;
        }
        state.input_cursor = acecode::clamp_utf8_boundary(
            state.input_text, state.input_cursor);
        if (state.input_cursor >= state.input_text.size()) return InputDisposition::Consumed;
        // 已知 [Pasted text #N] 占位符整体删除：连同 store 条目一起回收。
        if (auto span = acecode::tui::placeholder_starting_at(
                state.input_text, state.pasted_texts, state.input_cursor)) {
            state.pasted_texts.erase(span->paste_id);
            state.input_text.erase(span->begin, span->end - span->begin);
            // input_cursor 保持在 span->begin（即 input_cursor 不变）
            tui::refresh_input_suggestions(state, cmd_registry, ctx.turn.cwd());
            return InputDisposition::Consumed;
        }
        size_t next = state.input_cursor + 1;
        while (next < state.input_text.size() &&
               (static_cast<unsigned char>(state.input_text[next]) & 0xC0) == 0x80) {
            next++;
        }
        state.input_text.erase(state.input_cursor, next - state.input_cursor);
        tui::refresh_input_suggestions(state, cmd_registry, ctx.turn.cwd());
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

InputDisposition handle_composer_backspace(TuiInputContext& ctx, const ftxui::Event& event) {
    auto& state = ctx.state;
    auto& cmd_registry = ctx.commands;
    if (event == Event::Backspace) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.mode_picker_open) return InputDisposition::Consumed;
        state.input_vertical_goal_column.reset();
        if (acecode::erase_text_selection(
                state.input_text,
                state.input_cursor,
                state.input_selection_anchor)) {
            acecode::tui::prune_unreferenced(
                state.pasted_texts, state.input_text);
            tui::refresh_input_suggestions(
                state, cmd_registry, ctx.turn.cwd());
            return InputDisposition::Consumed;
        }
        state.input_cursor = acecode::clamp_utf8_boundary(
            state.input_text, state.input_cursor);
        if (state.input_text.empty()) {
            // On empty buffer, Backspace exits Shell mode back to Normal.
            if (state.input_mode == InputMode::Shell) {
                state.input_mode = InputMode::Normal;
            }
            state.input_cursor = 0;
            tui::refresh_input_suggestions(state, cmd_registry, ctx.turn.cwd());
            return InputDisposition::Consumed;
        }
        if (state.input_cursor == 0) {
            return InputDisposition::Consumed;
        }
        // 已知 [Pasted text #N] 占位符整体删除：连同 store 条目一起回收。
        if (auto span = acecode::tui::placeholder_ending_at(
                state.input_text, state.pasted_texts, state.input_cursor)) {
            state.pasted_texts.erase(span->paste_id);
            state.input_text.erase(span->begin, span->end - span->begin);
            state.input_cursor = span->begin;
            tui::refresh_input_suggestions(state, cmd_registry, ctx.turn.cwd());
            return InputDisposition::Consumed;
        }
        size_t pos = state.input_cursor - 1;
        // Walk back over UTF-8 continuation bytes (10xxxxxx)
        while (pos > 0 && (static_cast<unsigned char>(state.input_text[pos]) & 0xC0) == 0x80) {
            pos--;
        }
        state.input_text.erase(pos, state.input_cursor - pos);
        state.input_cursor = pos;
        tui::refresh_input_suggestions(state, cmd_registry, ctx.turn.cwd());
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

InputDisposition handle_composer_character(TuiInputContext& ctx, const ftxui::Event& event) {
    auto& state = ctx.state;
    auto& screen = ctx.screen;
    auto& viewport = ctx.viewport;
    auto& cmd_registry = ctx.commands;
    if (event.is_character()) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (auto result = tui::input_result(tui::list_picker_character_locked(state, screen, viewport, event))) return input_stopped(*result);

        const std::string ch = event.character();
        // Shell-mode trigger on an empty Normal buffer switches mode
        // without being inserted. Subsequent trigger characters are literal.
        if (state.input_mode == InputMode::Normal &&
            state.input_text.empty() &&
            is_shell_mode_trigger_character(ch)) {
            state.input_mode = InputMode::Shell;
            state.history_index = -1;
            return InputDisposition::Consumed;
        }
        acecode::insert_replacing_selection(
            state.input_text,
            state.input_cursor,
            state.input_selection_anchor,
            ch);
        acecode::tui::prune_unreferenced(
            state.pasted_texts, state.input_text);
        state.input_vertical_goal_column.reset();
        // Reset history browsing on new input
        state.history_index = -1;
        tui::refresh_input_suggestions(state, cmd_registry, ctx.turn.cwd());
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

}
