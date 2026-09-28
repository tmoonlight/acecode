#include "tui/app/tui_event_router.hpp"
#include "tui/input/event_prelude.hpp"
#include "tui/input/control_keys.hpp"
#include "tui/input/composer_pointer.hpp"
#include "tui/input/chat_keys.hpp"
#include "tui/input/mouse_router.hpp"
#include "tui/composer/paste.hpp"
#include "tui/composer/clipboard_keys.hpp"
#include "tui/composer/pending_attachment.hpp"
#include "tui/composer/submit.hpp"
#include "tui/composer/edit_keys.hpp"
#include "tui/overlays/ask_question_input.hpp"
#include "tui/overlays/confirm_overlay_input.hpp"
#include "tui/overlays/rewind_picker_input.hpp"
#include "tui/overlays/completion_dropdown_input.hpp"
#include "tui/overlays/list_picker_input.hpp"
#include <ftxui/component/component.hpp>

namespace acecode::tui {
const std::array<InputRoute, 38> & TuiEventRouter::routes() {
    // Do not group keys by subsystem: the original order is observable.
    static const std::array<InputRoute, 38> table{{
        {"prelude", 6846, handle_event_prelude},
        {"bracketed_paste", 6915, handle_bracketed_paste},
        {"ctrl_v", 6946, handle_clipboard_ctrl_v},
        {"alt_v", 6949, handle_clipboard_alt_v},
        {"ctrl_c", 6952, handle_ctrl_c},
        {"pending_attachment", 7000, handle_pending_attachment_input},
        {"composer_pointer", 7004, handle_composer_pointer},
        {"ask_guard", 7041, handle_ask_question_input},
        {"remote_confirm", 7064, pump_remote_confirm},
        {"confirm", 7080, handle_confirm_overlay_input},
        {"rewind", 7091, handle_rewind_picker_input},
        {"path_reference", 7095, handle_path_reference_input},
        {"slash", 7100, handle_slash_dropdown_input},
        {"enter", 7105, handle_composer_submit},
        {"picker_page", 7343, handle_list_picker_page},
        {"chat_page_up", 7400, handle_chat_page_up},
        {"chat_page_down", 7436, handle_chat_page_down},
        {"chat_alt_up", 7477, handle_chat_alt_up},
        {"chat_alt_down", 7489, handle_chat_alt_down},
        {"chat_home", 7501, handle_chat_home},
        {"chat_end", 7512, handle_chat_end},
        {"escape", 7521, handle_escape},
        {"tab", 7607, handle_tab},
        {"shift_tab", 7612, handle_shift_tab},
        {"mouse", 7636, handle_mouse},
        {"shift_arrow", 8196, handle_composer_shift_arrow},
        {"up", 8255, handle_composer_up},
        {"down", 8288, handle_composer_down},
        {"left", 8328, handle_composer_left},
        {"right", 8353, handle_composer_right},
        {"ctrl_a", 8394, handle_composer_ctrl_a},
        {"input_home", 8410, handle_composer_home},
        {"ctrl_o", 8421, handle_chat_ctrl_o},
        {"ctrl_e", 8433, handle_chat_ctrl_e},
        {"input_end", 8464, handle_composer_end},
        {"delete", 8473, handle_composer_delete},
        {"backspace", 8509, handle_composer_backspace},
        {"character", 8557, handle_composer_character},
    }};
    return table;
}

bool TuiEventRouter::handle(const ftxui::Event& event) {
    for (const auto& route : routes()) {
        if (const auto result = input_result(route.handler(context_, event))) {
            return *result;
        }
    }
    return false;
}

ftxui::Component TuiEventRouter::wrap(ftxui::Component child) {
    return ftxui::CatchEvent(std::move(child), [ref = lifetime_.ref(*this)](ftxui::Event event) {
        bool handled = false;
        ref.with([&](TuiEventRouter& router) { handled = router.handle(event); });
        return handled;
    });
}
}
