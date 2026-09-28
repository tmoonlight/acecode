#include "tui/input/tui_input_context.hpp"
#include "tui/overlays/completion_dropdown_input.hpp"
#include "tui/terminal_key_event.hpp"
#include "tui/picker_scroll.hpp"
#include <algorithm>
using ftxui::Event;
using ftxui::Mouse;
using ftxui::Box;
#include "tui/path_reference_input.hpp"

namespace acecode::tui {
static bool handle_slash_dropdown_event(TuiState& state,
                                        IScreenPort& screen,
                                        const Event& event) {
    std::unique_lock<std::mutex> lk(state.mu);
    if (!state.slash_dropdown_active || state.slash_dropdown_items.empty()) {
        return false;
    }

    auto commit_selection = [&]() {
        const auto& item =
            state.slash_dropdown_items[state.slash_dropdown_selected];
        state.input_text = "/" + item.name + " ";
        state.input_cursor = state.input_text.size();
        state.clear_input_selection();
        state.slash_dropdown_active = false;
        state.slash_dropdown_items.clear();
        state.slash_dropdown_selected = 0;
        state.slash_dropdown_view_offset = 0;
        state.slash_dropdown_total_matches = 0;
        state.slash_dropdown_dismissed_for_input = false;
    };
    const int n = static_cast<int>(state.slash_dropdown_items.size());
    auto follow_view = [&]() {
        state.slash_dropdown_view_offset =
            acecode::tui::scroll_to_keep_visible(
                state.slash_dropdown_selected,
                state.slash_dropdown_view_offset,
                acecode::tui::kSlashDropdownVisibleRows, n);
    };

    if (event == Event::ArrowUp ||
        tui::matches_terminal_codepoint(event, 'p', tui::kTerminalCtrl)) {
        state.slash_dropdown_selected =
            (state.slash_dropdown_selected - 1 + n) % n;
        follow_view();
        screen.post_event(Event::Custom);
        return true;
    }
    if (event == Event::ArrowDown ||
        tui::matches_terminal_codepoint(event, 'n', tui::kTerminalCtrl)) {
        state.slash_dropdown_selected =
            (state.slash_dropdown_selected + 1) % n;
        follow_view();
        screen.post_event(Event::Custom);
        return true;
    }
    if (event == Event::PageUp || event == Event::PageDown ||
        event == Event::Home || event == Event::End) {
        const int step = acecode::tui::kSlashDropdownVisibleRows;
        if (event == Event::PageUp) {
            state.slash_dropdown_selected =
                std::max(0, state.slash_dropdown_selected - step);
        } else if (event == Event::PageDown) {
            state.slash_dropdown_selected =
                std::min(n - 1, state.slash_dropdown_selected + step);
        } else if (event == Event::Home) {
            state.slash_dropdown_selected = 0;
        } else {
            state.slash_dropdown_selected = n - 1;
        }
        follow_view();
        screen.post_event(Event::Custom);
        return true;
    }
    if (event == Event::Tab) {
        commit_selection();
        screen.post_event(Event::Custom);
        return true;
    }
    if (event == Event::Return) {
        commit_selection();
        return false;
    }
    if (tui::matches_terminal_key(event, acecode::tui::TerminalKey::Escape)) {
        state.slash_dropdown_active = false;
        state.slash_dropdown_items.clear();
        state.slash_dropdown_selected = 0;
        state.slash_dropdown_view_offset = 0;
        state.slash_dropdown_total_matches = 0;
        state.slash_dropdown_dismissed_for_input = true;
        screen.post_event(Event::Custom);
        return true;
    }
    return false;
}

static bool handle_path_reference_event(
    TuiState& state,
    IScreenPort& screen,
    const Event& event,
    const std::string& cwd,
    const std::vector<Box>& row_boxes) {
    std::unique_lock<std::mutex> lk(state.mu);
    if (!state.path_reference_active) return false;

    const int count = static_cast<int>(state.path_reference_items.size());
    auto commit = [&](bool enter_directory) {
        if (!acecode::tui::commit_path_reference_selection(
                state, enter_directory)) {
            return false;
        }
        if (enter_directory) {
            acecode::tui::refresh_path_reference_state(state, cwd);
        }
        screen.post_event(Event::Custom);
        return true;
    };

    if (tui::matches_terminal_key(event, acecode::tui::TerminalKey::Escape)) {
        acecode::tui::dismiss_path_reference_state(state);
        screen.post_event(Event::Custom);
        return true;
    }
    if (count <= 0) return false;
    if (event == Event::ArrowUp) {
        acecode::tui::move_path_reference_selection(state, -1);
        screen.post_event(Event::Custom);
        return true;
    }
    if (event == Event::ArrowDown) {
        acecode::tui::move_path_reference_selection(state, 1);
        screen.post_event(Event::Custom);
        return true;
    }
    if (event == Event::PageUp || event == Event::PageDown) {
        const int delta = event == Event::PageUp
            ? -acecode::tui::kPathReferenceVisibleRows
            : acecode::tui::kPathReferenceVisibleRows;
        acecode::tui::move_path_reference_selection(state, delta);
        screen.post_event(Event::Custom);
        return true;
    }
    if (event == Event::Home || event == Event::End) {
        state.path_reference_selected = event == Event::Home ? 0 : count - 1;
        state.path_reference_view_offset = acecode::tui::scroll_to_keep_visible(
            state.path_reference_selected, state.path_reference_view_offset,
            acecode::tui::kPathReferenceVisibleRows, count);
        screen.post_event(Event::Custom);
        return true;
    }
    if (event == Event::Return) return commit(false);
    if (event == Event::ArrowRight || event == Event::Tab) {
        return commit(true);
    }
    if (event.is_mouse()) {
        const auto& mouse = event.mouse();
        if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) {
            return false;
        }
        for (int i = 0; i < count && i < static_cast<int>(row_boxes.size()); ++i) {
            const auto& box = row_boxes[static_cast<std::size_t>(i)];
            if (box.x_min <= box.x_max && box.y_min <= box.y_max &&
                box.Contain(mouse.x, mouse.y)) {
                state.path_reference_selected = i;
                return commit(false);
            }
        }
    }
    return false;
}

InputDisposition handle_slash_dropdown_input(TuiState& state, IScreenPort& screen,
    const ftxui::Event& event) {
    return input_handled(handle_slash_dropdown_event(state, screen, event));
}
InputDisposition handle_path_reference_input(TuiState& state, IScreenPort& screen,
    const ftxui::Event& event, const std::string& cwd, const std::vector<ftxui::Box>& row_boxes) {
    return input_handled(handle_path_reference_event(state, screen, event, cwd, row_boxes));
}

InputDisposition handle_path_reference_input(TuiInputContext& context, const ftxui::Event& event) {
    return handle_path_reference_input(context.state, context.screen, event, context.turn.cwd(), context.geometry.path_reference_boxes);
}

InputDisposition handle_slash_dropdown_input(TuiInputContext& context, const ftxui::Event& event) {
    return handle_slash_dropdown_input(context.state, context.screen, event);
}

}
