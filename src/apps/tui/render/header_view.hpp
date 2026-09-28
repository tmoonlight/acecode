#pragma once
#include "tui/tui_state.hpp"
#include <ftxui/dom/elements.hpp>

namespace acecode::tui {
ftxui::Element render_header_view(const TuiState& state,
    const std::string& version_str, const std::string& cwd_display,
    bool conhost_compat_layout, bool show_regular_sidebar, bool hide_regular_sidebar_banner);
}
