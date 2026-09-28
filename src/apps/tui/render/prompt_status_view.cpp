#include "tui/render/prompt_status_view.hpp"
#include "tui/theme_palette.hpp"
#include "tui/text_style.hpp"
#include <algorithm>
#include <utility>
#include <ftxui/screen/string.hpp>
using namespace ftxui;
#include "tui/render/status_chips.hpp"
#include "tui/render/frame_layout.hpp"
#include "tui/render/ask_question_style.hpp"
#include "tui/thinking_heartbeat.hpp"

namespace acecode::tui {
PromptStatusView render_prompt_status_view(const TuiState& state,
    const AskQuestionFrame& ask_question_frame, InputTextHitLayout& input_hit_layout,
    const PermissionManager& permissions, int terminal_width,
    bool show_regular_sidebar, bool conhost_compat_layout, bool dangerous_mode,
    const std::function<ftxui::Element()>& render_composer) {
    if (state.ask_pending || state.confirm_pending) input_hit_layout.clear();
    Element prompt_line;
    if (state.ask_pending) {
        // AskUserQuestion owns the complete inline editor. The ordinary composer
        // stays hidden even while the custom answer is active; paste and keyboard
        // events are adapted directly to the active session above.
        const auto ask_prompt_snapshot = state.ask_session
            ? state.ask_session->snapshot()
            : tui::AskQuestionSnapshot{};
        // Scroll hint applies to the question page only; the summary page
        // scrolls with the wheel and never needs the shortcut.
        const bool ask_scrollable =
            ask_prompt_snapshot.page != tui::AskQuestionPage::Summary &&
            ask_question_frame.layout.total_rows >
                ask_question_frame.layout.visible_rows;
        const auto ask_help_entries = tui::ask_question_help_entries(
            ask_prompt_snapshot, ask_scrollable);

        Elements ask_prompt_parts;
        ask_prompt_parts.push_back(
            text(" ? answering: ") | bold | color(tui::theme().ui.accent));
        if (!state.ask_origin_label.empty()) {
            // 子任务来源只在底部状态提示显示一次，避免与题目区域重复。
            ask_prompt_parts.push_back(
                text("[" + state.ask_origin_label + "] ") |
                color(tui::theme().ui.text_muted));
        }
        const int ask_help_width = std::max(
            20, terminal_width - 16 -
                    (show_regular_sidebar ? kRegularSidebarWidthCols : 0));
        // Reserve two rows: the footer shares the vertical stack with the chat
        // viewport, so a footer that changes height on state transitions also
        // moves the question panel (and with it the panel's visible row count).
        ask_prompt_parts.push_back(tui::build_ask_question_help_line(
            ask_help_entries, tui::ask_question_panel_colors(), ask_help_width,
            /*minimum_rows=*/2));
        prompt_line = hbox(std::move(ask_prompt_parts));
    } else if (state.confirm_pending) {
        // overlay 已经把选项画在 message_view 之上,prompt_line 仅作静态
        // 提示并吞掉字符输入(CatchEvent 中 confirm overlay handler 拦截非
        // 导航键,这里的 hbox 不渲染 input_with_esc 是为了让光标不在输入
        // 框里闪、误导用户去打字)。
        prompt_line = hbox({
            text(" [" + state.confirm_tool_name + "] ") | bold | color(tui::theme().syntax.preproc),
            text("awaiting confirmation \xE2\x80\x94 use \xE2\x86\x91\xE2\x86\x93 + Enter (Esc to deny)")
                | tui::readable_secondary(),
        });
    } else {
        Elements prompt_parts;
        if (state.input_mode == InputMode::Shell) {
            prompt_parts.push_back(text(" ! ") | bold | color(tui::theme().semantic.error));
        } else {
            prompt_parts.push_back(text(" > ") | bold | color(tui::theme().ui.border));
        }
        prompt_parts.push_back(
            render_composer() | flex |
                reflect(input_hit_layout.box));
        if (!state.pending_queue.empty()) {
            prompt_parts.push_back(
                text(" QUEUED " + std::to_string(state.pending_queue.size()) + " ") |
                bold | color(tui::theme().ui.text_primary) | bgcolor(tui::theme().ui.queued_bg));
        }
        prompt_line = hbox(std::move(prompt_parts));
    }

    // -- Bottom status bar --
    std::string perm_mode_str = std::string("mode: ") + PermissionManager::mode_name(permissions.mode());
    Element token_el = tui::render_token_usage_chip(state);
    Element load_el = tui::render_model_load_chip();
    Element goal_el = state.goal_status.empty()
        ? text("")
        : text("  " + state.goal_status + "  ") | dim | color(tui::theme().semantic.success);
    // 底部计时 chip(○ Thinking / ◑ Tool)已随 inline-thinking-heartbeat
    // change 删除:主进度元素在固定布局区,"被 overlay 遮挡/滚出视野"的
    // 前提不成立;等待期耗时+token 心跳改挂在推理指示行内联段。
    Element bottom_bar;
    if (conhost_compat_layout) {
        auto elapsed_secs = [](std::chrono::steady_clock::time_point start) -> long long {
            if (start.time_since_epoch().count() == 0) return 0;
            return static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count());
        };

        Elements status_parts;
        const bool show_ctrl_c_exit_hint =
            state.ctrl_c_armed && !state.is_waiting && !state.tool_running;
        if (show_ctrl_c_exit_hint) {
            status_parts.push_back(
                text("  Press ctrl+c again to exit  ") |
                tui::readable_secondary());
        } else if (dangerous_mode) {
            status_parts.push_back(
                text("  [YOLO]  ") | bold | color(tui::theme().ui.accent));
        } else if (state.is_waiting || state.tool_running) {
            status_parts.push_back(
                text("  esc / ctrl+c to interrupt  ") |
                tui::readable_secondary());
        } else {
            status_parts.push_back(
                text("  shift+tab: cycle permission mode  ") |
                tui::readable_secondary());
        }
        if (state.tool_running) {
            const long secs = elapsed_secs(state.tool_progress.start_time);
            status_parts.push_back(
                text("Tool: " + state.tool_progress.tool_name + " " +
                     std::to_string(secs) + "s  ") |
                bold | color(tui::theme().ui.accent));
        } else if (state.is_waiting) {
            // conhost 兼容布局不渲染 thinking_element,这行内联文本是该布局
            // 唯一的活性信号 —— 保留,但取数与主布局心跳段同源:回合累计
            // 已确认 + 当前请求估算,单调递增,不带 ~ 前缀。
            const long secs = elapsed_secs(state.thinking_start_time);
            std::string wait = "Thinking " + std::to_string(secs) + "s";
            const long long readout = state.turn_completion_tokens_confirmed +
                static_cast<long long>(state.streaming_output_chars / 4);
            if (readout > 0) {
                wait += " " + tui::format_token_count_short(readout) + " tok";
            }
            wait += "  ";
            status_parts.push_back(text(wait) | bold | color(tui::theme().ui.accent));
        }
        status_parts.push_back(goal_el);
        status_parts.push_back(token_el);
        status_parts.push_back(load_el);
        status_parts.push_back(text(perm_mode_str) |
                               tui::readable_secondary());
        bottom_bar = hbox(std::move(status_parts));
    } else if (state.ctrl_c_armed && !state.is_waiting && !state.tool_running) {
        bottom_bar = hbox({
            text("  Press ctrl+c again to exit") | tui::readable_secondary(),
            filler(),
            goal_el,
            token_el,
            load_el,
            text(perm_mode_str + "  ") | tui::readable_secondary(),
        });
    } else if (dangerous_mode) {
        bottom_bar = hbox({
            text("  [YOLO]") | bold | color(tui::theme().ui.accent),
            filler(),
            goal_el,
            token_el,
            load_el,
            text(perm_mode_str + "  ") | tui::readable_secondary(),
        });
    } else if (state.is_waiting || state.tool_running) {
        bottom_bar = hbox({
            text("  esc / ctrl+c to interrupt") | tui::readable_secondary(),
            filler(),
            goal_el,
            token_el,
            load_el,
            text(perm_mode_str + "  ") | tui::readable_secondary(),
        });
    } else {
        bottom_bar = hbox({
            text("  shift+tab: cycle permission mode") |
                tui::readable_secondary(),
            filler(),
            goal_el,
            token_el,
            load_el,
            text(perm_mode_str + "  ") | tui::readable_secondary(),
        });
    }

    return {std::move(prompt_line), std::move(bottom_bar)};
}

}
