#pragma once
#include "tui/input/ports.hpp"
#include "tui/tui_state.hpp"

namespace acecode::tui {
InputDisposition handle_confirm_overlay_input(TuiState& state, IScreenPort& screen,
    ftxui::Event& event, const PermissionResponder& respond_remote = nullptr);
}
