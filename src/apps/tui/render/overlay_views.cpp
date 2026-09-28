#include "tui/render/overlay_views.hpp"
#include "tui/theme_palette.hpp"
#include "tui/text_style.hpp"
#include <algorithm>
#include <utility>
#include <ftxui/screen/string.hpp>
using ftxui::Element;
using ftxui::Elements;
using ftxui::Box;
using ftxui::text;
using ftxui::vbox;
using ftxui::emptyElement;
using ftxui::size;
using ftxui::bold;
using ftxui::color;
using ftxui::bgcolor;
using ftxui::border;
using ftxui::focus;
#include "tui/render/frame_layout.hpp"
#include "tui/render/ask_question_style.hpp"
#include "tui/overlays/ask_session_projection.hpp"
#include "tui/confirm_question.hpp"

namespace acecode::tui {
OverlayViews render_overlay_views(TuiState& state, AskQuestionFrame& ask_question_frame,
    int terminal_width, int current_message_width, bool show_regular_sidebar, int viewport_rows) {
    // AskUserQuestion overlay —— 和 confirm_pending 互斥,在渲染层面
    // 显式让 ask 优先(事件层在 confirm 分支之前也已经拦截,这里只是
    // 作为显式护栏)。
    Element ask_overlay_element = emptyElement();
    ask_question_frame.reset_for_render();
    auto& ask_scrollbar_box = ask_question_frame.scrollbar_box;
    auto& ask_overlay_box = ask_question_frame.overlay_box;
    auto& ask_row_boxes = ask_question_frame.row_boxes;
    const auto ask_render_snapshot = state.ask_session
        ? std::optional<tui::AskQuestionSnapshot>(state.ask_session->snapshot())
        : std::nullopt;
    if (state.ask_pending && ask_render_snapshot.has_value() &&
        (ask_render_snapshot->page == tui::AskQuestionPage::Summary ||
         (ask_render_snapshot->current_question >= 0 &&
          ask_render_snapshot->current_question <
              ask_render_snapshot->total_questions))) {
        const int content_width =
            acecode::tui::ask_question_content_width_for_frame(
                terminal_width,
                current_message_width,
                show_regular_sidebar,
                kRegularSidebarWidthCols);
        const int max_visible_rows =
            std::max(1, viewport_rows - 2);

        if (state.ask_session) {
            const auto snapshot = state.ask_session->snapshot();
            const int layout_width = std::max(1, content_width);
            const int layout_height = max_visible_rows;
            tui::AskQuestionLayoutInput question_layout_input;
            question_layout_input.snapshot = &snapshot;
            question_layout_input.viewport_width = layout_width;
            question_layout_input.viewport_height = layout_height;
            question_layout_input.minimum_visible_rows =
                state.ask_config.min_visible_rows;
            question_layout_input.timeout_remaining_seconds =
                tui::ask_timeout_remaining_seconds(
                    *state.ask_session, std::chrono::steady_clock::now());
            // The transient status line is deliberately NOT injected here: it
            // belongs to the header/bottom status area. Rendering it as a panel
            // row pushed sibling notices (model switches and similar) inside the
            // question box and made the panel height change with unrelated
            // events.
            auto question_layout = tui::build_ask_question_layout(
                question_layout_input);
            ask_question_frame.layout = question_layout;
            // 布局会为焦点自动修正偏移；把该修正写回会话，避免下一帧
            // 重新从旧快照计算时出现滚动跳回。
            if (question_layout.scroll_offset != snapshot.scroll_offset) {
                const int max_scroll_offset = std::max(
                    0, question_layout.total_rows - question_layout.visible_rows);
                const auto scroll_effects = state.ask_session->dispatch({
                    tui::AskQuestionEventKind::SetScrollOffset,
                    -1,
                    question_layout.scroll_offset,
                    {},
                    0,
                    max_scroll_offset});
                tui::dispatch_ask_session_effects_locked(state, scroll_effects);
            }
            ask_question_frame.terminal_too_narrow =
                question_layout.terminal_too_narrow;
            ask_question_frame.row_boxes.assign(
                static_cast<std::size_t>(question_layout.visible_rows),
                Box{0, -1, 0, -1});

            tui::AskQuestionPanelInput panel_input;
            panel_input.layout = &question_layout;
            panel_input.snapshot = &snapshot;
            panel_input.colors = tui::ask_question_panel_colors();
            panel_input.terminal_too_narrow =
                question_layout.terminal_too_narrow;
            panel_input.row_boxes = &ask_row_boxes;
            panel_input.scrollbar_box = &ask_scrollbar_box;
            panel_input.overlay_box = &ask_overlay_box;
            ask_overlay_element = tui::build_ask_question_panel(panel_input);
        }
    }

    // Tool confirmation overlay —— 取代旧的单行 "y/a/n" 提示。
    // 三个固定选项:
    //   0  Yes
    //   1  Yes, allow all edits during this session (shift+tab)
    //   2  No
    // ↑↓ 移焦点,Enter 提交焦点项,1/2/3 数字键直选,Shift+Tab 直接选第二项,
    // Esc → Deny。事件分支在 CatchEvent 中,渲染层只是把状态画出来。
    Element confirm_overlay_element = emptyElement();
    if (state.confirm_pending) {
        Elements rows;
        if (!state.confirm_origin_label.empty()) {
            // 子会话的远程权限请求:标注来源,避免用户误以为是主会话工具。
            rows.push_back(text(" " + state.confirm_origin_label) |
                           tui::readable_secondary());
        }
        std::string title = acecode::tui::build_confirm_question(
            state.confirm_tool_name, state.confirm_tool_args);
        // build_confirm_question 可能返回多行(bash 把 command 附在第二行),
        // 按 \n 拆开逐行 push,首行加粗。
        bool first = true;
        size_t pos = 0;
        while (pos <= title.size()) {
            size_t nl = title.find('\n', pos);
            std::string line = (nl == std::string::npos)
                ? title.substr(pos)
                : title.substr(pos, nl - pos);
            if (first) {
                rows.push_back(text(" " + line) | bold | color(tui::theme().ui.accent));
                first = false;
            } else {
                rows.push_back(text(line) | color(tui::theme().ui.text_muted));
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        rows.push_back(text(""));

        const auto options = acecode::tui::build_confirm_options(
            state.confirm_tool_name, state.confirm_tool_args);
        const int option_count = std::max(1, static_cast<int>(options.size()));
        const int focus = std::clamp(state.confirm_focus, 0, option_count - 1);
        for (int i = 0; i < static_cast<int>(options.size()); ++i) {
            bool focused = (i == focus);
            std::string prefix = focused ? " \xE2\x9D\xAF " : "   ";
            auto row = text(prefix + options[static_cast<std::size_t>(i)].label);
            if (focused) {
                row = row | bold | color(tui::theme().ui.text_primary) | bgcolor(tui::theme().ui.selection_bg);
            } else {
                row = row | color(tui::theme().ui.text_muted);
            }
            rows.push_back(row);
        }
        rows.push_back(text(""));
        rows.push_back(
            text(" \xE2\x86\x91\xE2\x86\x93 move   Enter select   1-" + std::to_string(option_count) +
                 " jump   Esc deny")
            | tui::readable_secondary());
        confirm_overlay_element = vbox(std::move(rows)) | border | color(tui::theme().ui.accent);
    }

    return {std::move(ask_overlay_element), std::move(confirm_overlay_element)};
}

}
