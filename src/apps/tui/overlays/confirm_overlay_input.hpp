#pragma once
#include "tui/input/ports.hpp"
#include "tui/tui_state.hpp"

namespace acecode::tui {
struct TuiInputContext;
InputDisposition handle_confirm_overlay_input(TuiState& state, IScreenPort& screen,
    const ftxui::Event& event, const PermissionResponder& respond_remote = nullptr);
InputDisposition handle_confirm_overlay_input(TuiInputContext& context, const ftxui::Event& event);

InputDisposition pump_remote_confirm(TuiInputContext& context, const ftxui::Event& event);

}
