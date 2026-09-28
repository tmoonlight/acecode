#pragma once
#include "tui/input/ports.hpp"
#include "tui/tui_state.hpp"
#include "tui/chat/chat_viewport.hpp"

namespace acecode::tui {
struct TuiInputContext;
InputDisposition handle_rewind_picker_input(TuiState& state, IScreenPort& screen,
    const ftxui::Event& event, ChatViewport& viewport);
InputDisposition handle_rewind_picker_input(TuiInputContext& context, const ftxui::Event& event);

}
