#include "tui/overlays/ask_question_input.hpp"
#include "tui/terminal_key_event.hpp"
#include "tui/picker_scroll.hpp"
#include <algorithm>
using ftxui::Event;
using ftxui::Mouse;
using ftxui::Box;
#include "tui/overlays/ask_session_projection.hpp"
#include "tui/ask_question_text.hpp"
#include "tui/text_input_ops.hpp"

namespace acecode::tui {
static bool dispatch_ask_session_mouse_locked(
    TuiState& state,
    const Mouse& mouse,
    tui::AskQuestionFrame& ask_question_frame,
    IScreenPort& screen) {
    if (!state.ask_session) return false;
    auto& ask_question_layout = ask_question_frame.layout;
    auto& ask_scrollbar_box = ask_question_frame.scrollbar_box;
    auto& ask_overlay_box = ask_question_frame.overlay_box;
    const auto& ask_row_boxes = ask_question_frame.row_boxes;
    const bool wheel = mouse.button == Mouse::WheelUp ||
                       mouse.button == Mouse::WheelDown;
    const bool in_ask = tui::ask_question_box_contains(
                            ask_overlay_box, mouse.x, mouse.y) ||
                        tui::ask_question_box_contains(
                            ask_scrollbar_box, mouse.x, mouse.y);
    if (wheel) {
        if (!in_ask || ask_question_frame.terminal_too_narrow) return false;
        const int delta = mouse.button == Mouse::WheelUp ? -3 : 3;
        const int max_offset = std::max(
            0, ask_question_frame.layout.total_rows -
                ask_question_frame.layout.visible_rows);
        const auto effects = state.ask_session->dispatch({
            tui::AskQuestionEventKind::ScrollLines, -1, delta, {}, 0,
            max_offset});
        tui::dispatch_ask_session_effects_locked(state, effects);
        screen.post_event(Event::Custom);
        return true;
    }
    if (mouse.button == Mouse::Right && mouse.motion == Mouse::Pressed && in_ask) {
        const auto snapshot = state.ask_session->snapshot();
        if (snapshot.editing_custom && snapshot.editor.has_selection) {
            const auto effects = state.ask_session->dispatch({
                tui::AskQuestionEventKind::CopySelection});
            tui::dispatch_ask_session_effects_locked(state, effects);
        }
        return true;
    }
    if (mouse.button != Mouse::Left) return in_ask;
    const bool dragging_scrollbar = ask_question_frame.scrollbar_dragging;
    if (mouse.motion == Mouse::Moved && dragging_scrollbar) {
        const int track_height =
            ask_scrollbar_box.y_max - ask_scrollbar_box.y_min + 1;
        const int target = tui::ask_question_scroll_offset_for_track_y(
            mouse.y - ask_question_frame.scrollbar_grab_offset,
            ask_question_frame.scrollbar_box.y_min, track_height,
            ask_question_frame.layout.total_rows,
            ask_question_frame.layout.visible_rows);
        const int current = state.ask_session->snapshot().scroll_offset;
        const auto effects = state.ask_session->dispatch({
            tui::AskQuestionEventKind::ScrollLines, -1, target - current, {}, 0,
            std::max(0, ask_question_frame.layout.total_rows -
                            ask_question_frame.layout.visible_rows)});
        tui::dispatch_ask_session_effects_locked(state, effects);
        screen.post_event(Event::Custom);
        return true;
    }
    if (mouse.motion == Mouse::Released && dragging_scrollbar) {
        ask_question_frame.scrollbar_dragging = false;
        ask_question_frame.scrollbar_grab_offset = 0;
        return true;
    }
    if (mouse.motion == Mouse::Released && ask_question_frame.dragging_text) {
        ask_question_frame.dragging_text = false;
        ask_question_frame.press_target = {};
        return true;
    }
    if (mouse.motion == Mouse::Moved && ask_question_frame.dragging_text) {
        for (std::size_t visible = 0; visible < ask_row_boxes.size(); ++visible) {
            const auto& box = ask_row_boxes[visible];
            if (!box.Contain(mouse.x, mouse.y)) continue;
            const int row = ask_question_layout.scroll_offset +
                            static_cast<int>(visible);
            if (row < 0 || row >= static_cast<int>(ask_question_layout.rows.size()) ||
                ask_question_layout.rows[static_cast<std::size_t>(row)].kind !=
                    tui::AskQuestionLayoutKind::Custom) break;
            const auto snapshot = state.ask_session->snapshot();
            const auto& layout_row = ask_question_layout.rows[
                static_cast<std::size_t>(row)];
            if (!snapshot.editing_custom) break;
            // Custom answer text starts in the title column, not after the
            // number column: the marker column sits between them.
            const int text_x = box.x_min + ask_question_layout.title_x;
            const auto cursor = tui::ask_question_text_byte_offset_for_x(
                snapshot.editor.text,
                layout_row.text_byte_begin,
                layout_row.text_byte_end,
                mouse.x - text_x);
            const auto effects = state.ask_session->dispatch({
                tui::AskQuestionEventKind::MoveCursorTo, -1, 0, {}, cursor, -1,
                true});
            tui::dispatch_ask_session_effects_locked(state, effects);
            screen.post_event(Event::Custom);
            break;
        }
        return true;
    }
    if (!in_ask || ask_question_frame.terminal_too_narrow) return in_ask;
    if (mouse.motion == Mouse::Pressed) {
        ask_question_frame.press_x = mouse.x;
        ask_question_frame.press_y = mouse.y;
        ask_question_frame.press_target = {};
        if (ask_question_frame.layout.total_rows >
            ask_question_frame.layout.visible_rows &&
            tui::ask_question_box_contains(ask_scrollbar_box, mouse.x, mouse.y)) {
            ask_question_frame.scrollbar_dragging = true;
            ask_question_frame.scrollbar_grab_offset = std::clamp(
                mouse.y - (ask_scrollbar_box.y_min +
                           ask_question_layout.scrollbar_thumb.y),
                0, std::max(0, ask_question_layout.scrollbar_thumb.height - 1));
            const int track_height =
                ask_scrollbar_box.y_max - ask_scrollbar_box.y_min + 1;
            const int target = tui::ask_question_scroll_offset_for_track_y(
                mouse.y - ask_question_frame.scrollbar_grab_offset,
                ask_question_frame.scrollbar_box.y_min, track_height,
                ask_question_frame.layout.total_rows,
                ask_question_frame.layout.visible_rows);
            const int current = state.ask_session->snapshot().scroll_offset;
            const auto effects = state.ask_session->dispatch({
                tui::AskQuestionEventKind::ScrollLines, -1, target - current, {}, 0,
                std::max(0, ask_question_frame.layout.total_rows -
                               ask_question_frame.layout.visible_rows)});
            tui::dispatch_ask_session_effects_locked(state, effects);
            screen.post_event(Event::Custom);
            return true;
        }
        const auto target = tui::hit_test_ask_question_frame(
            ask_question_frame, mouse.x, mouse.y);
        ask_question_frame.press_target = target;
        if (target.kind == tui::AskQuestionHitKind::Custom) {
            tui::dispatch_ask_session_effects_locked(state, state.ask_session->dispatch({
                tui::AskQuestionEventKind::FocusOption, target.option_index}));
            tui::dispatch_ask_session_effects_locked(state, state.ask_session->dispatch({
                tui::AskQuestionEventKind::ToggleFocusedWithoutSubmit}));
            const auto snapshot = state.ask_session->snapshot();
            const auto visible_it = std::find_if(
                ask_row_boxes.begin(), ask_row_boxes.end(), [&](const Box& box) {
                    return box.Contain(mouse.x, mouse.y);
                });
            if (visible_it != ask_row_boxes.end()) {
                const int row = ask_question_layout.scroll_offset + static_cast<int>(
                    visible_it - ask_row_boxes.begin());
                if (row >= 0 && row < static_cast<int>(ask_question_layout.rows.size())) {
                    const auto& layout_row = ask_question_layout.rows[
                        static_cast<std::size_t>(row)];
                    const int text_x = visible_it->x_min +
                                       ask_question_layout.title_x;
                    const auto cursor = tui::ask_question_text_byte_offset_for_x(
                        snapshot.editor.text, layout_row.text_byte_begin,
                        layout_row.text_byte_end, mouse.x - text_x);
                    tui::dispatch_ask_session_effects_locked(state, state.ask_session->dispatch({
                        tui::AskQuestionEventKind::MoveCursorTo, -1, 0, {}, cursor}));
                }
            }
        ask_question_frame.dragging_text = true;
        }
        return true;
    }
    if (mouse.motion != Mouse::Released) return false;
    const auto press_target = ask_question_frame.press_target;
    const int target_kind = static_cast<int>(press_target.kind);
    const int question = press_target.question_index;
    const int option = press_target.option_index;
    const int dx = mouse.x - ask_question_frame.press_x;
    const int dy = mouse.y - ask_question_frame.press_y;
    ask_question_frame.press_target = {};
    if (target_kind == 0 || dx < -2 || dx > 2 || dy < -2 || dy > 2) return true;
    const auto kind = static_cast<tui::AskQuestionHitKind>(target_kind);
    const auto now = std::chrono::steady_clock::now();
    const bool same_click =
        ask_question_frame.last_click.kind == static_cast<tui::AskQuestionHitKind>(target_kind) &&
        ask_question_frame.last_click.question_index == question &&
        ask_question_frame.last_click.option_index == option &&
        ask_question_frame.last_click_at != std::chrono::steady_clock::time_point{} &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - ask_question_frame.last_click_at).count() <= 500;
    ask_question_frame.last_click_at = now;
    ask_question_frame.last_click = press_target;
    auto dispatch = [&](tui::AskQuestionEventKind kind_to_dispatch,
                        int event_option = -1) {
        tui::dispatch_ask_session_effects_locked(state, state.ask_session->dispatch({
            kind_to_dispatch, event_option, 0, {}}));
    };
    switch (kind) {
        case tui::AskQuestionHitKind::Option: {
            const auto before = state.ask_session->snapshot();
            dispatch(tui::AskQuestionEventKind::FocusOption, option);
            const bool selected = option >= 0 &&
                option < static_cast<int>(before.options.size()) &&
                before.options[static_cast<std::size_t>(option)].selected;
            if (same_click) {
                if (!selected) dispatch(tui::AskQuestionEventKind::ToggleFocused);
                dispatch(tui::AskQuestionEventKind::SubmitCurrentSelection);
                ask_question_frame.last_click_at = {};
            } else {
                dispatch(tui::AskQuestionEventKind::ToggleFocused);
            }
            break;
        }
        case tui::AskQuestionHitKind::Custom:
            dispatch(tui::AskQuestionEventKind::FocusOption, option);
            dispatch(tui::AskQuestionEventKind::ToggleFocusedWithoutSubmit);
            break;
        case tui::AskQuestionHitKind::SummaryQuestion:
            dispatch(tui::AskQuestionEventKind::OpenQuestion, question);
            break;
        default: break;
    }
    screen.post_event(Event::Custom);
    return true;
}

static bool dispatch_ask_session_event_locked(
    TuiState& state,
    const Event& event,
    const tui::AskQuestionFrame& ask_question_frame) {
    if (!state.ask_session || event == Event::Custom ||
        event.is_mouse() || event.is_cursor_position() ||
        event.is_cursor_shape()) {
        return false;
    }

    if (ask_question_frame.terminal_too_narrow) {
        if (tui::matches_terminal_key(event, acecode::tui::TerminalKey::Escape)) {
            const auto effects = state.ask_session->escape();
            tui::dispatch_ask_session_effects_locked(state, effects);
        }
        return true;
    }

    const bool editing_custom = state.ask_session->snapshot().editing_custom;
    const auto ask_snapshot = state.ask_session->snapshot();
    const bool custom_focused = !editing_custom &&
        ask_snapshot.focused_option == static_cast<int>(ask_snapshot.options.size());
    auto dispatch = [&](const tui::AskQuestionEvent& ask_event) {
        auto event_with_scroll_bound = ask_event;
        if (event_with_scroll_bound.kind == tui::AskQuestionEventKind::ScrollLines &&
            ask_question_frame.layout.total_rows >=
                ask_question_frame.layout.visible_rows) {
            event_with_scroll_bound.max_scroll_offset =
                std::max(0, ask_question_frame.layout.total_rows -
                                ask_question_frame.layout.visible_rows);
        }
        const auto effects = state.ask_session->dispatch(event_with_scroll_bound);
        tui::dispatch_ask_session_effects_locked(state, effects);
    };

    if (tui::matches_terminal_key(event, acecode::tui::TerminalKey::Escape)) {
        const auto effects = state.ask_session->escape();
        tui::dispatch_ask_session_effects_locked(state, effects);
        return true;
    }
    if (tui::matches_terminal_key(event, acecode::tui::TerminalKey::Enter, tui::kTerminalCtrl)) {
        dispatch({tui::AskQuestionEventKind::InsertNewline});
        return true;
    }
    if (tui::matches_terminal_key(event, acecode::tui::TerminalKey::ArrowUp, tui::kTerminalShift)) {
        dispatch({editing_custom ? tui::AskQuestionEventKind::SelectCursorUp
                                 : tui::AskQuestionEventKind::MoveUp});
        return true;
    }
    if (tui::matches_terminal_key(event, acecode::tui::TerminalKey::ArrowDown, tui::kTerminalShift)) {
        dispatch({editing_custom ? tui::AskQuestionEventKind::SelectCursorDown
                                 : tui::AskQuestionEventKind::MoveDown});
        return true;
    }
    if (tui::matches_terminal_key(event, acecode::tui::TerminalKey::ArrowLeft, tui::kTerminalShift)) {
        dispatch({editing_custom ? tui::AskQuestionEventKind::SelectCursorLeft
                                 : tui::AskQuestionEventKind::MoveLeft});
        return true;
    }
    if (tui::matches_terminal_key(event, acecode::tui::TerminalKey::ArrowRight, tui::kTerminalShift)) {
        dispatch({editing_custom ? tui::AskQuestionEventKind::SelectCursorRight
                                 : tui::AskQuestionEventKind::MoveRight});
        return true;
    }
    if (tui::matches_terminal_key(event, acecode::tui::TerminalKey::PageUp)) {
        const int page_step = std::max(
            1, ask_question_frame.layout.visible_rows - 1);
        dispatch({tui::AskQuestionEventKind::ScrollLines, -1, -page_step});
        return true;
    }
    if (tui::matches_terminal_key(event, acecode::tui::TerminalKey::PageDown)) {
        const int page_step = std::max(
            1, ask_question_frame.layout.visible_rows - 1);
        dispatch({tui::AskQuestionEventKind::ScrollLines, -1, page_step});
        return true;
    }
    if (tui::matches_terminal_key(event, acecode::tui::TerminalKey::Tab, tui::kTerminalShift)) {
        dispatch({editing_custom ? tui::AskQuestionEventKind::InsertText
                                 : tui::AskQuestionEventKind::PreviousPage,
                  -1, 0, editing_custom ? "\t" : ""});
        return true;
    }
    if (tui::matches_terminal_key(event, acecode::tui::TerminalKey::Tab)) {
        dispatch({editing_custom ? tui::AskQuestionEventKind::InsertText
                                 : tui::AskQuestionEventKind::NextPage,
                  -1, 0, editing_custom ? "\t" : ""});
        return true;
    }
    if (tui::matches_terminal_codepoint(event, 'x', tui::kTerminalShift)) {
        dispatch({tui::AskQuestionEventKind::GlobalCancel});
        return true;
    }
    if (tui::matches_terminal_codepoint(event, 'x', tui::kTerminalCtrl)) {
        dispatch({tui::AskQuestionEventKind::CutSelection});
        return true;
    }
    if (tui::matches_terminal_codepoint(event, 'y')) {
        if (editing_custom || custom_focused) {
            dispatch({tui::AskQuestionEventKind::InsertText, -1, 0, "y"});
        } else {
            dispatch({tui::AskQuestionEventKind::CopyFocused});
        }
        return true;
    }
    if (event == Event::ArrowUp) {
        dispatch({editing_custom ? tui::AskQuestionEventKind::MoveCursorUp
                                 : tui::AskQuestionEventKind::MoveUp});
        return true;
    }
    if (event == Event::ArrowDown) {
        dispatch({editing_custom ? tui::AskQuestionEventKind::MoveCursorDown
                                 : tui::AskQuestionEventKind::MoveDown});
        return true;
    }
    if (event == Event::ArrowLeft) {
        dispatch({editing_custom ? tui::AskQuestionEventKind::MoveCursorLeft
                                 : tui::AskQuestionEventKind::MoveLeft});
        return true;
    }
    if (event == Event::ArrowRight) {
        dispatch({editing_custom ? tui::AskQuestionEventKind::MoveCursorRight
                                 : tui::AskQuestionEventKind::MoveRight});
        return true;
    }
    if (event == Event::Return) {
        dispatch({tui::AskQuestionEventKind::SubmitFocused});
        return true;
    }
    if (event == Event::Character(' ')) {
        dispatch({editing_custom ? tui::AskQuestionEventKind::InsertText
                                 : tui::AskQuestionEventKind::ToggleFocused,
                  -1, 0, editing_custom ? " " : ""});
        return true;
    }
    if (event == Event::Backspace) {
        dispatch({tui::AskQuestionEventKind::Backspace});
        return true;
    }
    if (event == Event::Delete) {
        dispatch({tui::AskQuestionEventKind::DeleteForward});
        return true;
    }
    if (event == Event::Home) {
        dispatch({tui::AskQuestionEventKind::MoveCursorHome});
        return true;
    }
    if (event == Event::End) {
        dispatch({tui::AskQuestionEventKind::MoveCursorEnd});
        return true;
    }
    if (event == Event::Character('j')) {
        dispatch({editing_custom || custom_focused
                      ? tui::AskQuestionEventKind::InsertText
                      : tui::AskQuestionEventKind::MoveDown,
                  -1, 0, editing_custom || custom_focused ? "j" : ""});
        return true;
    }
    if (event == Event::Character('k')) {
        dispatch({editing_custom || custom_focused
                      ? tui::AskQuestionEventKind::InsertText
                      : tui::AskQuestionEventKind::MoveUp,
                  -1, 0, editing_custom || custom_focused ? "k" : ""});
        return true;
    }
    if (event.is_character()) {
        dispatch(acecode::tui::ask_question_character_event(
            event.character(), editing_custom));
        return true;
    }
    return false;
}

InputDisposition handle_ask_question_input(TuiState& state, IScreenPort& screen,
    ftxui::Event& event, AskQuestionFrame& ask_question_frame) {
    // AskQuestionSession owns all question interaction semantics. The TUI
    // layer only adapts raw events, performs side effects, and projects
    // snapshots for rendering. Keep this guard before the shared input
    // handlers so no session event can reach another input state machine.
    {
        std::unique_lock<std::mutex> lk(state.mu);
        if (state.ask_pending && state.ask_session) {
            if (event == Event::Custom || event.is_cursor_position()) {
                return InputDisposition::Declined;
            }
            if (dispatch_ask_session_event_locked(
                    state, event, ask_question_frame)) {
                screen.post_event(Event::Custom);
                return InputDisposition::Consumed;
            }
            if (event.is_mouse()) {
                return input_stopped(dispatch_ask_session_mouse_locked(
                    state, event.mouse(), ask_question_frame, screen));
            }
            return InputDisposition::Consumed;
        }
    }


    return InputDisposition::Continue;
}

}
