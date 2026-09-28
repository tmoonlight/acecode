#include "tui/render/frame_renderer.hpp"
#include "tui/render/prepare_frame.hpp"
#include "tui/render/frame_layout.hpp"
#include "tui/render/header_view.hpp"
#include "tui/render/transcript_view.hpp"
#include "tui/render/activity_indicator_view.hpp"
#include "tui/render/picker_views.hpp"
#include "tui/render/overlay_views.hpp"
#include "tui/render/prompt_status_view.hpp"
#include "tui/render/link_hover_tooltip.hpp"
#include "tui/render/regular_sidebar_view.hpp"
#include "tui/render/status_chips.hpp"
#include "tui/path_reference_dropdown.hpp"
#include "tui/slash_dropdown.hpp"
#include "tui/todo_checklist_view.hpp"
#include "tui/ask_question_panel.hpp"
#include "tui/ask_question_view.hpp"
#include "tui/theme_palette.hpp"
#include <ftxui/screen/terminal.hpp>
#include <algorithm>
#include <utility>
using ftxui::Element;
using ftxui::Color;
using ftxui::text;
using ftxui::hbox;
using ftxui::vbox;
using ftxui::dbox;
using ftxui::emptyElement;
using ftxui::size;
using ftxui::flex;
using ftxui::color;
using ftxui::borderRounded;
using ftxui::separator;
using ftxui::separatorHeavy;
using ftxui::separatorLight;

namespace acecode::tui {
TuiFrameRenderer::TuiFrameRenderer(TuiState& state, IScreenPort& screen,
    const std::string& version_str, const std::string& cwd_display,
    ChatViewport& viewport, FrameGeometry& geometry, std::atomic<int>& anim_tick,
    ftxui::Component input_component, PermissionManager& permissions,
    bool dangerous_mode, bool conhost_compat_layout, bool hover_supported,
    TerminalSize terminal_size)
    : state_(state), screen_(screen), version_str_(version_str), cwd_display_(cwd_display),
      viewport_(viewport), geometry_(geometry), anim_tick_(anim_tick),
      input_component_(std::move(input_component)), permissions_(permissions),
      dangerous_mode_(dangerous_mode), conhost_compat_layout_(conhost_compat_layout),
      hover_supported_(hover_supported), terminal_size_(std::move(terminal_size)) {
    if (!terminal_size_) terminal_size_ = [] {
        const auto size = ftxui::Terminal::Size();
        return FrameTerminalSize{size.dimx, size.dimy};
    };
}

ftxui::Element TuiFrameRenderer::render() {
    auto& state = state_;
    auto& screen = screen_;
    auto& viewport = viewport_;
    auto& geometry = geometry_;
    auto& version_str = version_str_;
    auto& cwd_display = cwd_display_;
    auto& anim_tick = anim_tick_;
    auto& input_with_esc = input_component_;
    auto& permissions = permissions_;
    const bool conhost_compat_layout = conhost_compat_layout_;
    const bool dangerous_mode = dangerous_mode_;
    const bool hover_supported = hover_supported_;
    auto& ask_question_frame = geometry.ask_question_frame;
    auto& input_hit_layout = geometry.input_hit_layout;
    auto& path_reference_boxes = geometry.path_reference_boxes;
    auto& sidebar_content_box = geometry.sidebar_content_box;
    auto& sidebar_viewport_box = geometry.sidebar_viewport_box;
    auto& sidebar_scrollbar_box = geometry.sidebar_scrollbar_box;
    std::lock_guard<std::mutex> lk(state.mu);
    input_hit_layout.clear();
    auto compat_horizontal_line = [&terminal_size = terminal_size_] {
        const int cols = terminal_size().width;
        const int safe_cols = std::max(1, cols > 4 ? cols - 4 : cols);
        const std::string glyph = "\xE2\x94\x80";
        std::string line;
        line.reserve(glyph.size() * static_cast<size_t>(safe_cols));
        for (int i = 0; i < safe_cols; ++i) {
            line += glyph;
        }
        return text(line);
    };
    constexpr int kRegularSidebarWidthCols = tui::kRegularSidebarWidthCols;
    const int terminal_width =
        std::max(terminal_size_().width, screen.dimx());
    const auto prepared = prepare_frame_locked(state, screen, viewport, geometry,
        terminal_width, conhost_compat_layout);
    const int current_message_width = prepared.current_message_width;
    const int markdown_render_width = prepared.markdown_render_width;
    const bool show_regular_sidebar = prepared.show_regular_sidebar;
    const bool hide_regular_sidebar_banner = prepared.hide_regular_sidebar_banner;

    Element header = tui::render_header_view(state, version_str, cwd_display,
        conhost_compat_layout, show_regular_sidebar, hide_regular_sidebar_banner);

    Element message_view = tui::render_transcript_view(state, viewport, geometry,
        current_message_width, markdown_render_width, conhost_compat_layout);

    auto activity = tui::render_activity_indicator_view(
        state, conhost_compat_layout, show_regular_sidebar, anim_tick.load());
    Element thinking_element = std::move(activity.thinking);
    Element mcp_loading_element = std::move(activity.mcp_loading);

    auto pickers = tui::render_picker_views(state);
    Element resume_picker_element = std::move(pickers.resume);
    Element rewind_picker_element = std::move(pickers.rewind);
    Element model_picker_element = std::move(pickers.model);
    Element mode_picker_element = std::move(pickers.mode);

    Element path_reference_element =
        acecode::tui::render_path_reference_dropdown(
            state, conhost_compat_layout, path_reference_boxes);
    Element slash_dropdown_element =
        render_slash_dropdown(state, conhost_compat_layout);

    auto overlays = tui::render_overlay_views(state, ask_question_frame,
        terminal_width, current_message_width, show_regular_sidebar, viewport.rows());
    Element ask_overlay_element = std::move(overlays.ask);
    Element confirm_overlay_element = std::move(overlays.confirm);

    auto prompt_status = tui::render_prompt_status_view(state, ask_question_frame,
        input_hit_layout, permissions, terminal_width, show_regular_sidebar,
        conhost_compat_layout, dangerous_mode,
        [&input_with_esc] { return input_with_esc->Render(); });
    Element prompt_line = std::move(prompt_status.prompt);
    Element bottom_bar = std::move(prompt_status.status);

    // IME composition window positioning is handled by FTXUI's cursor
    // system (focusCursorBlock) which emits ANSI sequences to place the
    // terminal cursor at the caret. Windows Terminal/ConPTY uses this
    // to position the IME window. The Win32 IME APIs (ImmSetComposition
    // Window) don't work under ConPTY.

    Color outer_border_color = (state.input_mode == InputMode::Shell)
        ? acecode::tui::theme().semantic.error
        : acecode::tui::theme().ui.text_muted;

    Element header_separator = hide_regular_sidebar_banner
        ? emptyElement()
        : (conhost_compat_layout ? compat_horizontal_line() : separatorHeavy());
    Element prompt_separator = conhost_compat_layout
        ? compat_horizontal_line()
        : separatorLight();
    const int pending_queue_width = current_message_width > 0
        ? current_message_width
        : std::max(20, terminal_width -
            (show_regular_sidebar ? kRegularSidebarWidthCols + 6 : 4));
    Element pending_queue_element =
        tui::render_pending_queue_block(state, pending_queue_width);
    Element pending_attachment_element =
        tui::render_pending_attachment_block(state, pending_queue_width);
    Element todo_checklist_element =
        acecode::tui::todo_checklist_uses_sidebar(show_regular_sidebar)
            ? emptyElement()
            : acecode::tui::render_todo_checklist_block(
                state.todos, pending_queue_width);

    Element main_root = vbox({
        header,
        header_separator | color(acecode::tui::theme().ui.text_dim),
        acecode::tui::compose_ask_question_message_area(
            std::move(message_view),
            std::move(ask_overlay_element),
            state.ask_pending) | flex,
        mcp_loading_element,
        resume_picker_element,
        rewind_picker_element,
        model_picker_element,
        mode_picker_element,
        confirm_overlay_element,
        path_reference_element,
        slash_dropdown_element,
        thinking_element,
        todo_checklist_element,
        pending_queue_element,
        prompt_separator | color(acecode::tui::theme().ui.text_dim),
        pending_attachment_element,
        prompt_line,
        bottom_bar,
    });

    Element root;
    if (conhost_compat_layout) {
        root = vbox({
            compat_horizontal_line() | color(outer_border_color),
            main_root | flex,
            compat_horizontal_line() | color(outer_border_color),
        }) | flex;
    } else if (show_regular_sidebar) {
        Element sidebar = acecode::tui::render_regular_sidebar(
            state,
            version_str,
            cwd_display,
            kRegularSidebarWidthCols,
            anim_tick.load(),
            sidebar_content_box,
            sidebar_viewport_box,
            sidebar_scrollbar_box);
        root = hbox({
            main_root | flex,
            separator() | color(outer_border_color),
            sidebar,
        }) | borderRounded | color(outer_border_color) | flex;
    } else {
        root = main_root | borderRounded | color(outer_border_color) | flex;
    }

    // link-hover-tooltip (add-tui-hyperlinks 5.3): 气泡作为浮层叠加在整屏
    // 之上 —— dbox 共享区域,不参与布局(不挤压任何元素)、不捕获输入。
    // 仅当终端能力探测通过(会收到无按键 Mouse::Moved 事件)且气泡已显示
    // 时注入;conhost 家族/Apple Terminal.app 等恒不渲染。state.mu 由本
    // 函数入口持有,读 hover_link_* 安全。
    if (hover_supported && state.hover_link_visible &&
        !state.hover_link_href.empty()) {
        const auto hover_size = terminal_size_();
        root = dbox({
            std::move(root),
            tui::render_link_hover_tooltip(state, hover_size.width, hover_size.height),
        });
    }
    return root;
}

}
