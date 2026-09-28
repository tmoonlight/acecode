#include "frame_layout.hpp"
#include <algorithm>

namespace acecode::tui {

FrameLayout compute_frame_layout(int terminal_width, bool conhost_compat_layout,
    int previous_chat_box_width) {
    const bool show_regular_sidebar =
        !conhost_compat_layout && terminal_width > kRegularSidebarThresholdCols;
    const int fallback_message_width = terminal_width -
        (show_regular_sidebar ? kRegularSidebarWidthCols + 9 : 6);
    const int markdown_render_width =
        std::max(20, (previous_chat_box_width > 0
            ? previous_chat_box_width : fallback_message_width) - 6);
    return {show_regular_sidebar, markdown_render_width};
}

} // namespace acecode::tui
