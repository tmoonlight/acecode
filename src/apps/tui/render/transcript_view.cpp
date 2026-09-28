#include "tui/render/transcript_view.hpp"
#include "tui/theme_palette.hpp"
#include "tui/text_style.hpp"
#include <algorithm>
#include <utility>
#include <ftxui/screen/string.hpp>
using ftxui::Element;
using ftxui::Elements;
using ftxui::Color;
using ftxui::paragraph;
using ftxui::vbox;
using ftxui::emptyElement;
using ftxui::size;
using ftxui::HEIGHT;
using ftxui::EQUAL;
using ftxui::flex;
using ftxui::color;
using ftxui::reflect;
using ftxui::focusPosition;
using ftxui::focusPositionRelative;
using ftxui::yframe;
using ftxui::selectionBackgroundColor;
using ftxui::selectionForegroundColor;
#include "tui/chat/message_render_revision.hpp"
#include "tui/render/message_row_views.hpp"
#include "tui/render/tool_row_view.hpp"
#include "tui/chat_message_spacing.hpp"
#include "tui/chat_scroll.hpp"
#include "tui/thick_vscroll_bar.hpp"
#include "tui/unclipped_reflect.hpp"
#include "platform/terminal/terminal_capability.hpp"

namespace acecode::tui {
ftxui::Element render_transcript_view(const TuiState& state, ChatViewport& viewport,
    FrameGeometry& geometry, int current_message_width, int markdown_render_width,
    bool conhost_compat_layout) {
    auto& chat_box = viewport.chat_box;
    auto& scrollbar_box = geometry.scrollbar_box;
    auto& message_boxes = geometry.message_boxes;
    auto& chat_link_regions = geometry.chat_link_regions;
    auto& message_render_cache = viewport.message_render_cache;
    auto& message_layout_boxes = viewport.message_layout_boxes;
    auto& message_layout_valid = viewport.message_layout_valid;
    auto& message_layout_revisions = viewport.message_layout_revisions;
    auto& message_layout_widths = viewport.message_layout_widths;
    auto& message_line_counts = viewport.message_line_counts;
    auto& message_spacer_rows_after = viewport.message_spacer_rows_after;
    // -- Messages --
    // Bottom-anchor short transcripts while following the tail. FTXUI's
    // yframe only scrolls when the child is taller than the viewport, so a
    // short chat at tail otherwise remains pinned to the top.
    Elements message_elements;
    auto push_spacer_rows = [&message_elements](int rows) {
        if (rows > 0) {
            message_elements.push_back(
                emptyElement() | size(HEIGHT, EQUAL, rows));
        }
    };
    const int chat_viewport_height = chat_box.y_max >= chat_box.y_min
        ? chat_box.y_max - chat_box.y_min + 1
        : 0;
    const int tail_top_padding =
        acecode::tui::chat_bottom_anchor_top_padding_rows(
        message_line_counts,
        static_cast<int>(state.conversation.size()),
        chat_viewport_height,
        message_spacer_rows_after);
    push_spacer_rows(tail_top_padding);
    const auto render_window = acecode::tui::chat_render_window(
        message_line_counts,
        static_cast<int>(state.conversation.size()),
        state.chat_scroll_top_row,
        chat_viewport_height,
        acecode::tui::default_chat_render_overscan_rows(
            chat_viewport_height),
        message_spacer_rows_after);
    push_spacer_rows(render_window.top_spacer_rows);

    // drag-autoscroll: 每条消息同时记录两个 box:
    //   - message_layout_boxes: 未裁剪高度,用于滚动数学;
    //   - message_boxes: 裁剪后屏幕位置,用于选区锚点补偿。
    auto tracked_message = [&](size_t index, Element element) -> Element {
        if (index < message_layout_valid.size()) {
            message_layout_valid[index] = 1;
            message_layout_revisions[index] = tui::message_render_revision(
                state.conversation[index], state.transcript_expanded);
            message_layout_widths[index] = current_message_width;
        }
        return std::move(element)
            | acecode::tui::reflect_unclipped(message_layout_boxes[index])
            | reflect(message_boxes[index]);
    };
    auto render_message_markdown =
        [&](const std::string& content, Color fallback_color) -> Element {
        try {
            acecode::markdown::FormatOptions md_opts;
            md_opts.terminal_width = markdown_render_width;
            md_opts.syntax_highlight = true;
            md_opts.hyperlinks = true;
            // OSC 8 原生超链接(add-tui-hyperlinks 5.2):按终端探测结果开启。
            // static 缓存避免 Windows 上每帧重复跑 console probe;TUI 渲染
            // 单线程,magic static 初始化安全。
            static const bool osc8_supported =
                acecode::detect_osc8_support();
            md_opts.osc8_hyperlinks = osc8_supported;
            md_opts.strip_xml = true;
            md_opts.link_regions = &chat_link_regions;
            return acecode::markdown::format_markdown(content, md_opts);
        } catch (...) {
            return paragraph(content) | color(fallback_color);
        }
    };
    // L1 消息级 Element 缓存:内容与渲染上下文(宽度/主题/语法)不变时复用
    // 上帧构建的 Element,跳过 format_markdown。Ruling R6:仅缓存无链接
    // 消息 —— format_markdown 会给链接元素 bake reflect(region.box),
    // 其指向每帧 clear() 的 collector 区域,跨帧复用会悬垂;无链接消息无
    // reflect 装饰器,可安全复用。含链接消息永不缓存,走原全量路径。
    auto render_cached_message_markdown =
        [&](size_t index, const std::string& content,
            Color fallback_color) -> Element {
        // Content participates only in the render cache, never the layout revision.
        const std::size_t rev = tui::message_render_cache_revision(
            state.conversation[index], state.transcript_expanded, content);
        const acecode::tui::MessageRenderCacheKey cache_key{
            rev, current_message_width,
            acecode::tui::theme_palette_version(), /*syntax=*/true};
        if (message_render_cache.valid(index, cache_key)) {
            return *message_render_cache.element(index);
        }
        const std::size_t links_before = chat_link_regions.regions().size();
        Element element = render_message_markdown(content, fallback_color);
        const bool has_links =
            chat_link_regions.regions().size() > links_before;
        if (!has_links) {
            message_render_cache.store(
                index, cache_key, element,
                std::vector<acecode::tui::CachedLinkRegion>{});
        }
        return element;
    };
    const size_t render_first =
        static_cast<size_t>(std::max(0, render_window.first_message));
    const size_t render_last = std::min(
        state.conversation.size(),
        static_cast<size_t>(std::max(0,
            render_window.last_message_exclusive)));
    // 工具行元数据只扫描可见窗口；若窗口切进并行工具批次，纯函数会扩到
    // 相邻批次边界，单次 FIFO 同时得到 call 灯态和 result 工具名。
    const auto tool_metadata =
        acecode::tui::compute_tool_row_metadata_window(
            state.conversation, render_first, render_last);
    for (size_t i = render_first; i < render_last; ++i) {
        const auto& msg = state.conversation[i];
        const std::string& paired_tool_name =
            tool_metadata.result_name_at(i);
        const bool task_complete_result =
            acecode::tui::is_task_complete_result(
                msg, paired_tool_name);
        bool focused_message = static_cast<int>(i) == state.chat_focus_index;
        Element row;
        if (msg.role == "user") {
            row = render_user_message_row(msg, focused_message);
        } else if (msg.role == "assistant") {
            row = render_assistant_message_row(
                render_cached_message_markdown(i, msg.content, theme().semantic.success),
                focused_message);
        } else if (msg.role == "tool_call") {
            row = render_tool_call_row(msg, tool_metadata.call_dot_at(i),
                state.transcript_expanded, focused_message);
        } else if (msg.role == "user_shell_output") {
            row = render_tool_text_row(
                render_tool_result_lines_preserving_breaks(msg.content), focused_message);
        } else if (task_complete_result) {
            row = render_tool_text_row(render_cached_message_markdown(i,
                task_complete_summary_markdown(msg), theme().ui.text_primary), focused_message);
        } else if (msg.ask_result) {
            row = render_tool_text_row(
                render_tool_result_lines_preserving_breaks(msg.content), focused_message);
        } else if (msg.role == "tool_result") {
            row = render_tool_result_row(msg, chat_box, state.transcript_expanded, focused_message);
        } else {
            row = render_notice_message_row(msg, state.transcript_expanded, focused_message);
        }
        if (row) message_elements.push_back(tracked_message(i, std::move(row)));
        push_spacer_rows(acecode::tui::chat_spacer_rows_after_at(
            message_spacer_rows_after, static_cast<int>(i)));
    }
    push_spacer_rows(render_window.bottom_spacer_rows);

    Element message_body = vbox(std::move(message_elements));
    if (state.chat_follow_tail) {
        // /resume replaces the transcript before the next render has
        // measured the new paragraph heights. Use FTXUI's rendered bottom
        // anchor while tail-follow is active so the first restored frame
        // lands at the true tail instead of an underestimated absolute row.
        message_body = message_body | focusPositionRelative(0.0f, 1.0f);
    } else {
        const int frame_focus_y =
            acecode::tui::chat_frame_focus_y_for_scroll_top(
                state.chat_scroll_top_row, viewport.rows());
        message_body = message_body | focusPosition(0, frame_focus_y);
    }

    // draggable-thick-scrollbar: thumb glyph identical to upstream
    // vscroll_indicator (┃╹╻),painted only in the rightmost reserved
    // column. We reserve 3 columns total so the *invisible* hit zone
    // is wider than 1 cell — the leftmost 2 reserved columns render
    // as whitespace but scrollbar_box.Contain() still matches them,
    // making mouse aim much easier without changing the visual rail
    // position. The decorator also enforces a minimum thumb height
    // (3 cells) so long sessions don't shrink the click target to a
    // single half-block.
    auto message_view = acecode::tui::thick_vscroll_bar(
                            std::move(message_body),
                            /*width=*/3,
                            scrollbar_box,
                            conhost_compat_layout)
        | yframe | reflect(chat_box) | flex
        // mouse-selection-copy: visual feedback for drag-selection. The
        // decorator lives on the message_view so selection can span
        // multiple messages.
        | selectionBackgroundColor(tui::theme().ui.selection_bg)
        | selectionForegroundColor(tui::theme().ui.selection_fg);

    return message_view;
}

}
