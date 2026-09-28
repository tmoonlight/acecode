#pragma once
#include "tui/tui_state.hpp"
#include <ftxui/dom/elements.hpp>
#include <optional>

namespace acecode::tui {
struct LinkHoverPlacement { std::string url; int x, y, width, height; };
std::optional<LinkHoverPlacement> place_link_hover_tooltip(const TuiState& state,
    int terminal_width, int terminal_height);
ftxui::Element render_link_hover_tooltip(const TuiState& state,
    int terminal_width, int terminal_height);
}
