#pragma once
#include "tui/input/ports.hpp"
#include "tui/tui_state.hpp"
#include <ftxui/screen/box.hpp>

namespace acecode::tui {
InputDisposition handle_slash_dropdown_input(TuiState& state, IScreenPort& screen,
    ftxui::Event& event);
InputDisposition handle_path_reference_input(TuiState& state, IScreenPort& screen,
    ftxui::Event& event, const std::string& cwd, const std::vector<ftxui::Box>& row_boxes);
}
