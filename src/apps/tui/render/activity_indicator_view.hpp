#pragma once
#include "tui/tui_state.hpp"
#include <ftxui/dom/elements.hpp>

namespace acecode::tui {
struct ActivityIndicatorView { ftxui::Element thinking; ftxui::Element mcp_loading; };
ActivityIndicatorView render_activity_indicator_view(const TuiState& state,
    bool conhost_compat_layout, bool show_regular_sidebar, int anim_tick);
}
