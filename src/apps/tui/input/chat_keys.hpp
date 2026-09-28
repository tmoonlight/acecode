#pragma once
#include "tui/input/ports.hpp"
#include "tui/tui_state.hpp"
#include "tui/chat/chat_viewport.hpp"

namespace acecode::tui {
struct TuiInputContext;
InputDisposition handle_chat_ctrl_e(TuiState& state, IScreenPort& screen,
    ChatViewport& viewport, const ftxui::Event& event);
InputDisposition handle_chat_page_up(TuiInputContext& context, const ftxui::Event& event);
InputDisposition handle_chat_page_down(TuiInputContext& context, const ftxui::Event& event);
InputDisposition handle_chat_alt_up(TuiInputContext& context, const ftxui::Event& event);
InputDisposition handle_chat_alt_down(TuiInputContext& context, const ftxui::Event& event);
InputDisposition handle_chat_home(TuiInputContext& context, const ftxui::Event& event);
InputDisposition handle_chat_end(TuiInputContext& context, const ftxui::Event& event);
InputDisposition handle_chat_ctrl_o(TuiInputContext& context, const ftxui::Event& event);
InputDisposition handle_chat_ctrl_e(TuiInputContext& context, const ftxui::Event& event);
}
