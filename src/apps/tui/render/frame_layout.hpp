#pragma once
namespace acecode::tui {
inline constexpr int kRegularSidebarThresholdCols = 120;
inline constexpr int kRegularSidebarWidthCols = 43;
struct FrameLayout {
    bool show_regular_sidebar;
    int markdown_render_width;
};
FrameLayout compute_frame_layout(int terminal_width, bool conhost_compat_layout,
    int previous_chat_box_width);
}
