#pragma once
#include "tui/input/ports.hpp"
#include "tui/tui_state.hpp"
#include "tui/chat/chat_viewport.hpp"

namespace acecode::tui {
struct TuiInputContext;
// The *_locked entry points borrow the caller-held state.mu lock.
InputDisposition list_picker_enter_locked(TuiState& state, IScreenPort& screen, ChatViewport& viewport);
InputDisposition list_picker_page(TuiState& state, IScreenPort& screen, const ftxui::Event& event);
InputDisposition list_picker_escape_locked(TuiState& state, IScreenPort& screen, ChatViewport& viewport);
InputDisposition list_picker_up_locked(TuiState& state, IScreenPort& screen);
InputDisposition list_picker_down_locked(TuiState& state, IScreenPort& screen);
InputDisposition list_picker_character_locked(TuiState& state, IScreenPort& screen, ChatViewport& viewport, const ftxui::Event& event);
InputDisposition handle_list_picker_page(TuiInputContext& context, const ftxui::Event& event);

}
