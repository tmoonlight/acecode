#pragma once
#include "tui/chat/chat_viewport.hpp"
#include "tui/render/frame_geometry.hpp"
#include "tui/screen_port.hpp"

namespace acecode::tui {
struct PreparedFrame {
    int current_message_width;
    int markdown_render_width;
    bool show_regular_sidebar;
    bool hide_regular_sidebar_banner;
};
// state.mu is held throughout prepare and all following view construction.
PreparedFrame prepare_frame_locked(TuiState& state, IScreenPort& screen,
    ChatViewport& viewport, FrameGeometry& geometry, int terminal_width,
    bool conhost_compat_layout);
}
