#pragma once
#include "tui/input/ports.hpp"
#include "tui/tui_state.hpp"
#include "tui/chat/chat_viewport.hpp"

namespace acecode::tui {
InputDisposition handle_rewind_picker_input(TuiState& state, IScreenPort& screen,
    ftxui::Event& event, ChatViewport& viewport);
}
