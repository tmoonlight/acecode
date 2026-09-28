#pragma once
#include "tui/input/ports.hpp"
#include "tui/tui_state.hpp"
#include <ftxui/screen/box.hpp>

namespace acecode::tui {
struct TuiInputContext;
InputDisposition handle_slash_dropdown_input(TuiState& state, IScreenPort& screen,
    const ftxui::Event& event);
InputDisposition handle_path_reference_input(TuiState& state, IScreenPort& screen,
    const ftxui::Event& event, const std::string& cwd, const std::vector<ftxui::Box>& row_boxes);
InputDisposition handle_path_reference_input(TuiInputContext& context, const ftxui::Event& event);

InputDisposition handle_slash_dropdown_input(TuiInputContext& context, const ftxui::Event& event);

}
