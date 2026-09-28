#include "tui/render/prepare_frame.hpp"
#include "tui/render/frame_layout.hpp"
#include "tui/input/input_trace.hpp"
#include "utils/logger.hpp"
using ftxui::Box;

namespace acecode::tui {
PreparedFrame prepare_frame_locked(TuiState& state, IScreenPort& screen,
    ChatViewport& viewport, FrameGeometry& geometry, int terminal_width,
    bool conhost_compat_layout) {
    auto& chat_box = viewport.chat_box;
    auto& sidebar_content_box = geometry.sidebar_content_box;
    auto& sidebar_viewport_box = geometry.sidebar_viewport_box;
    auto& sidebar_scrollbar_box = geometry.sidebar_scrollbar_box;
    auto& message_boxes = geometry.message_boxes;
    auto& chat_link_regions = geometry.chat_link_regions;
    auto& message_layout_boxes = viewport.message_layout_boxes;
    auto& message_layout_valid = viewport.message_layout_valid;
    auto& message_layout_revisions = viewport.message_layout_revisions;
    auto& message_layout_widths = viewport.message_layout_widths;
    const auto frame_layout = tui::compute_frame_layout(
        terminal_width, conhost_compat_layout,
        chat_box.x_max >= chat_box.x_min ? chat_box.x_max - chat_box.x_min + 1 : 0);
    const bool show_regular_sidebar = frame_layout.show_regular_sidebar;
    if (!show_regular_sidebar) {
        sidebar_content_box = Box{1, 0, 1, 0};
        sidebar_viewport_box = Box{1, 0, 1, 0};
        sidebar_scrollbar_box = Box{1, 0, 1, 0};
        state.sidebar_scroll_top_row = 0;
        state.sidebar_scrollbar_dragging = false;
        state.sidebar_scrollbar_grab_offset_2x = 0;
    }
    const bool hide_regular_sidebar_banner =
        show_regular_sidebar && !state.conversation.empty();

    // drag-autoscroll: 把上一帧布局分配的未裁剪 box 高度同步到行数表,
    // 供 scroll_chat_by_lines 做按行滚动. 普通 ftxui::reflect 会在 Render
    // 阶段和 screen.stencil 取交集,只能拿到可见高度;这里必须用未裁剪高度,
    // 否则长消息会被误判为只剩当前可见的几行,导致底部滚动范围过短.
    size_t n_msgs = state.conversation.size();
    const int current_message_width = chat_box.x_max >= chat_box.x_min
        ? chat_box.x_max - chat_box.x_min + 1 : 0;
    const int markdown_render_width = frame_layout.markdown_render_width;
    viewport.sync_from_layout(state);
    viewport.clamp_focus(state);

    // selection-anchor-compensation: 在清空 boxes 之前,先用上一帧 reflect 的
    // box.y_min 检测 anchor 漂移。focus_index 和 line_offset 都没变(用户没动
    // 滚轮 / PgUp / 拖滚动条)而 y_min 变了 —— 这种纯 layout 漂移期间如果用户
    // 在拖选,FTXUI 的 selection_data_ 钉在物理屏幕坐标会错位,这里调
    // ShiftSelection 把锚点跟随移到新位置。ShiftSelection 内部会在没有 active
    // selection 时 early return,这里无条件调用是安全的。
    {
        int cur_focus = state.chat_focus_index;
        int cur_offset = state.chat_line_offset;
        int cur_y = (cur_focus >= 0 && cur_focus < (int)message_boxes.size())
            ? message_boxes[cur_focus].y_min : 0;
        bool focus_unchanged = (cur_focus == state.last_focus_index &&
                                 cur_offset == state.last_chat_line_offset);
        // last_focus_box_y 的 sentinel 是 -999999 (从未拍过),用 > -1000000
        // 判断"已有有效快照"。cur_y 在 reflect 没回填时是 0(default Box),
        // 也跳过补偿——避免被裁出 viewport 的 anchor 触发假阳性 dy。
        if (focus_unchanged && cur_y > 0 &&
            state.last_focus_box_y > -1000000 &&
            cur_y != state.last_focus_box_y) {
            int dy = cur_y - state.last_focus_box_y;
            ACECODE_INPUT_TRACE(
            LOG_DEBUG("[drag-select] anchor compensation dy=" +
                      std::to_string(dy) + " focus=" +
                      std::to_string(cur_focus) + " offset=" +
                      std::to_string(cur_offset) + " y=" +
                      std::to_string(state.last_focus_box_y) + "->" +
                      std::to_string(cur_y));
            );
            screen.shift_selection(0, dy);
        }
        // 仅在拿到有效 reflect 数据时更新快照,否则保留上次的;这样 anchor
        // 短暂被裁出 viewport 再回到可见区时,差值仍是相对最近一次可见位置。
        if (cur_y > 0) {
            state.last_focus_box_y = cur_y;
        }
        state.last_focus_index = cur_focus;
        state.last_chat_line_offset = cur_offset;
    }

    message_boxes.assign(n_msgs, Box{});
    chat_link_regions.clear();
    message_layout_boxes.assign(n_msgs, Box{});
    message_layout_valid.assign(n_msgs, 0);
    message_layout_revisions.assign(n_msgs, 0);
    message_layout_widths.assign(n_msgs, 0);

    return {current_message_width, markdown_render_width,
        show_regular_sidebar, hide_regular_sidebar_banner};
}

}
