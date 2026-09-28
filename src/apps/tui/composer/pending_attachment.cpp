#include "tui/composer/pending_attachment.hpp"
#include "tui/pending_attachment_selection.hpp"
#include "tui/model/status_line.hpp"
#include "tui/terminal_key_event.hpp"
#include "session/composer_attachments.hpp"
using ftxui::Event;

namespace acecode::tui {
static bool handle_pending_attachment_focus_event(TuiState& state,
                                                  IScreenPort& screen,
                                                  const Event& event) {
    std::lock_guard<std::mutex> lk(state.mu);

    const bool unavailable =
        state.ask_pending ||
        state.confirm_pending ||
        state.resume_picker_active ||
        state.rewind_picker_active ||
        state.model_picker_open ||
        state.mode_picker_open;

    if (tui::is_alt_a_event(event)) {
        if (unavailable) {
            return true;
        }
        if (state.pending_attachments.empty()) {
            state.pending_attachment_focus =
                acecode::tui::kNoPendingAttachmentFocus;
            tui::set_transient_status_line_locked(state, "No pending attachments");
        } else {
            acecode::tui::toggle_pending_attachment_focus(
                state.pending_attachment_focus,
                state.pending_attachments.size());
        }
        screen.post_event(Event::Custom);
        return true;
    }

    if (unavailable ||
        !acecode::tui::has_pending_attachment_focus(
            state.pending_attachment_focus,
            state.pending_attachments.size())) {
        return false;
    }

    if (tui::matches_terminal_key(event, acecode::tui::TerminalKey::Escape)) {
        state.pending_attachment_focus =
            acecode::tui::kNoPendingAttachmentFocus;
        screen.post_event(Event::Custom);
        return true;
    }
    if (event == Event::ArrowUp || event == Event::ArrowDown) {
        const int delta = (event == Event::ArrowUp) ? -1 : 1;
        acecode::tui::move_pending_attachment_focus(
            state.pending_attachment_focus,
            state.pending_attachments.size(),
            delta);
        screen.post_event(Event::Custom);
        return true;
    }
    if (event == Event::Backspace || event == Event::Delete) {
        auto removed_index =
            acecode::tui::remove_focused_pending_attachment_index(
                state.pending_attachment_focus,
                state.pending_attachments.size());
        if (removed_index.has_value()) {
            const auto index = *removed_index;
            const nlohmann::json removed = state.pending_attachments[index];
            const std::string kind =
                removed.value("kind", std::string{"attachment"});
            const std::string label =
                kind == "image" ? "Removed image: " : "Removed attachment: ";
            state.pending_attachments.erase(
                state.pending_attachments.begin() +
                static_cast<std::ptrdiff_t>(index));
            tui::set_transient_status_line_locked(
                state,
                label + attachment_name_from_json(removed));
        }
        screen.post_event(Event::Custom);
        return true;
    }
    if (event.is_character()) {
        state.pending_attachment_focus =
            acecode::tui::kNoPendingAttachmentFocus;
    }
    return false;
}

InputDisposition handle_pending_attachment_input(TuiInputContext& ctx, const ftxui::Event& event) {
    return input_handled(handle_pending_attachment_focus_event(ctx.state, ctx.screen, event));
}

}
