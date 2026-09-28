#pragma once
#include "tui/chat/chat_viewport.hpp"
#include "tui/render/frame_geometry.hpp"

namespace acecode::tui {
ftxui::Element render_transcript_view(const TuiState& state, ChatViewport& viewport,
    FrameGeometry& geometry, int current_message_width, int markdown_render_width,
    bool conhost_compat_layout);
}
