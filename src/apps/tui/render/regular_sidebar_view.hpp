#pragma once
#include "tui/tui_state.hpp"
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

namespace acecode::tui {
// Caller holds state.mu. Run at root layout time: this view clamps and writes
// sidebar_scroll_top_row using the previous reflected content/viewport boxes.
ftxui::Element render_regular_sidebar(TuiState& state,
    const std::string& version_str, const std::string& cwd_display,
    int sidebar_width, int anim_tick, ftxui::Box& content_box,
    ftxui::Box& viewport_box, ftxui::Box& scrollbar_box);
} // namespace acecode::tui
