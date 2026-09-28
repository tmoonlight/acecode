#include "tui/input/tui_input_context.hpp"
#include "tui/overlays/rewind_picker_input.hpp"
#include "tui/terminal_key_event.hpp"
#include "tui/picker_scroll.hpp"
#include <algorithm>
using ftxui::Event;
using ftxui::Mouse;
using ftxui::Box;
#include "tui/overlays/rewind_picker_model.hpp"

namespace acecode::tui {
static void clear_rewind_picker_locked(TuiState& state) {
    state.rewind_picker_active = false;
    state.rewind_mode_active = false;
    state.rewind_picker_operation = TuiState::RewindPickerOperation::Rewind;
    state.rewind_items.clear();
    state.rewind_selected = 0;
    state.rewind_view_offset = 0;
    state.rewind_modes.clear();
    state.rewind_mode_selected = 0;
    state.rewind_callback = nullptr;
}

static void commit_rewind_mode_locked(
    TuiState& state,
    IScreenPort& screen,
    tui::ChatViewport& viewport,
    TuiState::RewindRestoreMode mode) {
    if (state.rewind_selected < 0 ||
        state.rewind_selected >= static_cast<int>(state.rewind_items.size())) {
        clear_rewind_picker_locked(state);
        screen.post_event(Event::Custom);
        return;
    }
    auto item = state.rewind_items[state.rewind_selected];
    auto cb = state.rewind_callback;
    clear_rewind_picker_locked(state);
    state.input_text.clear();
    state.pasted_texts.clear();
    state.input_cursor = 0;
    state.clear_input_selection();
    if (cb) cb(std::move(item), mode);
    viewport.clamp_focus(state);
    screen.post_event(Event::Custom);
}

static bool handle_rewind_picker_event(
    TuiState& state,
    IScreenPort& screen,
    const Event& event,
    tui::ChatViewport& viewport) {
    std::unique_lock<std::mutex> lk(state.mu);
    if (!state.rewind_picker_active) {
        return false;
    }

    auto activate_current_target = [&]() {
        if (state.rewind_selected < 0 ||
            state.rewind_selected >= static_cast<int>(state.rewind_items.size())) {
            return;
        }
        const auto& item = state.rewind_items[state.rewind_selected];
        if (TuiState::rewind_target_uses_mode_picker(
                state.rewind_picker_operation, item.can_restore_code)) {
            tui::populate_rewind_modes_locked(state, item);
            state.rewind_mode_active = true;
        } else {
            commit_rewind_mode_locked(
                state, screen, viewport,
                TuiState::RewindRestoreMode::ConversationOnly);
        }
    };

    if (tui::matches_terminal_key(event, acecode::tui::TerminalKey::Escape)) {
        if (state.rewind_mode_active) {
            state.rewind_mode_active = false;
            state.rewind_modes.clear();
            state.rewind_mode_selected = 0;
        } else {
            const bool is_fork =
                state.rewind_picker_operation ==
                TuiState::RewindPickerOperation::Fork;
            clear_rewind_picker_locked(state);
            state.conversation.push_back({
                "system",
                is_fork ? "Fork cancelled." : "Rewind cancelled.",
                false});
            state.chat_follow_tail = true;
            viewport.clamp_focus(state);
        }
        screen.post_event(Event::Custom);
        return true;
    }

    const bool move_up =
        event == Event::ArrowUp || event == Event::Character('k');
    const bool move_down =
        event == Event::ArrowDown || event == Event::Character('j');
    if (move_up || move_down) {
        int* selected = state.rewind_mode_active
            ? &state.rewind_mode_selected
            : &state.rewind_selected;
        int count = state.rewind_mode_active
            ? static_cast<int>(state.rewind_modes.size())
            : static_cast<int>(state.rewind_items.size());
        if (count > 0) {
            *selected = move_up
                ? (*selected - 1 + count) % count
                : (*selected + 1) % count;
        }
        if (!state.rewind_mode_active) {
            state.rewind_view_offset = acecode::tui::scroll_to_keep_visible(
                state.rewind_selected, state.rewind_view_offset,
                acecode::tui::kRewindPickerVisibleRows,
                static_cast<int>(state.rewind_items.size()));
        }
        screen.post_event(Event::Custom);
        return true;
    }

    if (!state.rewind_mode_active &&
        (event == Event::PageUp || event == Event::PageDown ||
         event == Event::Home || event == Event::End)) {
        const int total = static_cast<int>(state.rewind_items.size());
        if (total > 0) {
            const int step = acecode::tui::kRewindPickerVisibleRows;
            if (event == Event::PageUp) {
                state.rewind_selected = std::max(0, state.rewind_selected - step);
            } else if (event == Event::PageDown) {
                state.rewind_selected =
                    std::min(total - 1, state.rewind_selected + step);
            } else if (event == Event::Home) {
                state.rewind_selected = 0;
            } else {
                state.rewind_selected = total - 1;
            }
            state.rewind_view_offset = acecode::tui::scroll_to_keep_visible(
                state.rewind_selected, state.rewind_view_offset, step, total);
            screen.post_event(Event::Custom);
        }
        return true;
    }

    if (event == Event::Return) {
        if (state.rewind_mode_active) {
            if (state.rewind_mode_selected >= 0 &&
                state.rewind_mode_selected <
                    static_cast<int>(state.rewind_modes.size())) {
                auto mode = state.rewind_modes[state.rewind_mode_selected].mode;
                commit_rewind_mode_locked(
                    state, screen, viewport, mode);
            }
        } else {
            activate_current_target();
        }
        screen.post_event(Event::Custom);
        return true;
    }

    if (event.is_character()) {
        std::string ch = event.character();
        if (!ch.empty() && ch[0] >= '1' && ch[0] <= '9') {
            int idx = ch[0] - '1';
            if (state.rewind_mode_active) {
                if (idx < static_cast<int>(state.rewind_modes.size())) {
                    state.rewind_mode_selected = idx;
                    auto mode = state.rewind_modes[idx].mode;
                    commit_rewind_mode_locked(
                        state, screen, viewport, mode);
                }
            } else if (idx < static_cast<int>(state.rewind_items.size())) {
                state.rewind_selected = idx;
                activate_current_target();
            }
            screen.post_event(Event::Custom);
        }
        return true;
    }

    return true;
}

InputDisposition handle_rewind_picker_input(TuiState& state, IScreenPort& screen,
    const ftxui::Event& event, ChatViewport& viewport) {
    return input_handled(handle_rewind_picker_event(state, screen, event, viewport));
}

InputDisposition handle_rewind_picker_input(TuiInputContext& context, const ftxui::Event& event) {
    return handle_rewind_picker_input(context.state, context.screen, event, context.viewport);
}

}
