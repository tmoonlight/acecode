#include "tui/composer/clipboard_keys.hpp"
#include "tui/composer/paste.hpp"
#include "tui/model/status_line.hpp"
#include "tui/terminal_key_event.hpp"
#include "utils/logger.hpp"
using ftxui::Event;
using ftxui::Mouse;

namespace acecode::tui {
InputDisposition handle_clipboard_ctrl_v(TuiInputContext& ctx, const ftxui::Event& event) {
    if (tui::matches_terminal_codepoint(event, 'v', tui::kTerminalCtrl)) {
        return input_stopped(paste_clipboard_text(ctx));
    }
    return InputDisposition::Continue;
}

InputDisposition handle_clipboard_alt_v(TuiInputContext& ctx, const ftxui::Event& event) {
    if (tui::is_alt_v_event(event)) {
        return input_stopped(paste_clipboard_image(ctx));
    }
    return InputDisposition::Continue;
}

InputDisposition handle_clipboard_right_click(TuiInputContext& ctx, const ftxui::Event& event) {
    auto mouse_event = event; // Read-only mouse access through the FTXUI value API.
    auto& state = ctx.state;
    auto& screen = ctx.screen;
    if (event.is_mouse() && mouse_event.mouse().button == Mouse::Right && mouse_event.mouse().motion == Mouse::Pressed) {
        std::string sel = screen.get_selection();
        if (sel.empty()) {
            return input_stopped(paste_clipboard_text(ctx));
        }
        auto clipboard_write = ctx.clipboard.write_text(sel);
        std::string status_msg;
        if (clipboard_write) {
            status_msg = "Copied " + std::to_string(sel.size()) +
                         " bytes to clipboard";
            LOG_INFO("Copied " + std::to_string(sel.size()) +
                     " bytes to clipboard via system clipboard");
        } else if (
            clipboard_write.status != ClipboardTextWriteResult::Status::TooLarge) {
            ctx.clipboard.write_osc52(sel);
            status_msg = "Sent OSC 52 copy request";
            LOG_INFO("Sent OSC 52 copy request for " +
                     std::to_string(sel.size()) + " bytes");
        } else {
            status_msg = tui::clipboard_copy_status_message(clipboard_write.status);
            LOG_WARN(status_msg);
        }
        {
            std::lock_guard<std::mutex> lk(state.mu);
            tui::set_transient_status_line_locked(state, status_msg);
        }
        screen.post_event(Event::Custom);
        return InputDisposition::Consumed;
    }
    return InputDisposition::Continue;
}

}
