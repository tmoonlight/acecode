#pragma once
#include "tui/input/ports.hpp"
#include "tui/tui_state.hpp"
#include "tui/chat/chat_viewport.hpp"

namespace acecode::tui {
InputDisposition handle_chat_ctrl_e(TuiState& state, IScreenPort& screen,
    ChatViewport& viewport, const ftxui::Event& event);
}
