#include "tui/input/composer_pointer.hpp"
#include "tui/composer/suggestions.hpp"
#include "tui/pending_attachment_selection.hpp"

using ftxui::Event;
using ftxui::Mouse;
using ftxui::Box;

namespace acecode::tui {
InputDisposition handle_composer_pointer(TuiInputContext& context, const Event& event) {
    auto& state = context.state;
    auto& screen = context.screen;
    auto& input_hit_layout = context.geometry.input_hit_layout;
    auto& cmd_registry = context.commands;
    if (event.is_mouse()) {
        const auto& mouse = event.mouse();
        bool ask_session_active = false;
        {
            std::lock_guard<std::mutex> lk(state.mu);
            ask_session_active = state.ask_pending && state.ask_session;
        }
        if (!ask_session_active &&
            mouse.button == Mouse::Left &&
            mouse.motion == Mouse::Pressed) {
            std::lock_guard<std::mutex> lk(state.mu);
            const auto press =
                acecode::tui::resolve_input_pointer_press(
                    state, input_hit_layout, mouse.x, mouse.y);
            if (press.cursor_placed) {
                state.input_cursor = press.cursor_bytes;
                state.input_selection_anchor.reset();
                state.input_vertical_goal_column.reset();
                state.pending_attachment_focus =
                    acecode::tui::kNoPendingAttachmentFocus;
                if (press.target ==
                    acecode::tui::InputPointerTarget::Composer) {
                    tui::refresh_input_suggestions(
                        state, cmd_registry, context.turn.cwd());
                }
                screen.post_event(Event::Custom);
                // FTXUI must see the same Pressed event to establish the
                // anchor used by its existing drag-selection state machine.
                return input_stopped(press.event_consumed);
            }
        }
    }
    return InputDisposition::Continue;
}

}
