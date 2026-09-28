#include "tui/render/tool_row_view.hpp"
#include "tui/diff_view.hpp"
#include "tui/tool_result_fold.hpp"
#include "tui/tool_row_presentation.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <vector>
#include <array>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>

#include "tui/tui_state.hpp"
#include "tui/text_style.hpp"
#include "tui/theme_palette.hpp"
#include "tui/text_truncation.hpp"
#include "tui/sidebar_model.hpp"
#include "tui/non_selectable.hpp"
#include "tui/pending_attachment_selection.hpp"
#include "tui/thick_vscroll_bar.hpp"
#include "tui/terminal_key_event.hpp"
#include "tui/todo_checklist_view.hpp"
#include "tui/unclipped_reflect.hpp"
#include "tui/vertical_scroll.hpp"
#include "session/token_tracker.hpp"
#include "tui/text_input_ops.hpp"
#include "tool/mcp_manager.hpp"
#include "lsp/lsp_service.hpp"

using ftxui::Element;
using ftxui::Elements;
using ftxui::Color;
using ftxui::text;
using ftxui::paragraph;
using ftxui::hbox;
using ftxui::vbox;
using ftxui::size;
using ftxui::flex;
using ftxui::dim;
using ftxui::bold;
using ftxui::color;
using ftxui::focus;

#include "tui/render/text_cells.hpp"
#include "tui/render/status_chips.hpp"
#include "tui/model/mcp_sidebar_model.hpp"

namespace acecode::tui {
bool is_success_summary(const ToolSummary& s) {
    for (const auto& kv : s.metrics) {
        if (kv.first == "exit" && kv.second != "0") return false;
        if (kv.first == "aborted" && kv.second == "true") return false;
        if (kv.first == "timeout" && kv.second == "true") return false;
    }
    return true;
}

std::string renderable_tool_summary_line(const ToolSummary& s,
                                         const std::string& metric_str,
                                         int max_visual_width) {
    // Claude Code 风格结果行不带图标:工具名已在上方的 `● ToolName(args)` 行
    // 加粗展示,这里的图标只是噪音(file_read 的 "→" 图标还会和已移除的
    // 箭头前缀撞脸)。verb + object + metrics 足够辨识。
    const std::string prefix = s.verb + " \xC2\xB7 ";
    const std::string suffix = metric_str.empty()
        ? std::string()
        : " \xC2\xB7 " + metric_str;
    return truncate_middle_segment(prefix, s.object, suffix, max_visual_width);
}

Element render_tool_result_lines_preserving_breaks(const std::string& display_content) {
    Elements lines;
    size_t pos = 0;
    while (pos <= display_content.size()) {
        const size_t nl = display_content.find('\n', pos);
        const std::string line = (nl == std::string::npos)
            ? display_content.substr(pos)
            : display_content.substr(pos, nl - pos);
        Element line_el = line.empty() ? text(" ") : paragraph(line);
        lines.push_back(line_el | color(theme().ui.text_muted) | dim);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return vbox(std::move(lines));
}


ftxui::Element render_tool_call_row(const TuiState::Message& msg, ToolCallDot tool_dot,
    bool transcript_expanded, bool focused_message) {
    // 紧凑工具行:` ● ToolName`;Ctrl+O 全局 verbose 开启时才追加
    // `(args)`。指示灯按配对结果着色(灰=执行中/无结果、绿=成功、
    // 红=失败),工具名 PascalCase 加粗。content 解析失败时折叠态
    // 只显示 ToolCall,verbose 态才回退到完整原文。
    const auto parts = acecode::tui::parse_tool_row(
        msg.content, msg.display_override);
    const auto& palette = tui::theme();
    const bool show_args = acecode::tui::tool_call_arguments_visible(
        transcript_expanded);
    Color dot_color = tui::theme().ui.text_dim;
    if (tool_dot == acecode::tui::ToolCallDot::Ok) {
        dot_color = tui::theme().semantic.success;
    } else if (tool_dot == acecode::tui::ToolCallDot::Failed) {
        dot_color = tui::theme().semantic.error;
    }
    Elements segs;
    segs.push_back(text(" \xE2\x97\x8F ") | color(dot_color)); // "●"
    if (parts.name.empty()) {
        if (show_args) {
            segs.push_back(paragraph(msg.content)
                | color(acecode::tui::tool_call_argument_color(palette))
                | flex);
        } else {
            segs.push_back(text("ToolCall") | bold |
                color(acecode::tui::tool_call_name_color(palette)));
        }
    } else {
        const std::string display_name =
            acecode::tui::pascal_case_tool_name(parts.name);
        segs.push_back(
            text(display_name)
            | bold | color(acecode::tui::tool_call_name_color(palette)));
        if (show_args && !parts.args.empty()) {
            segs.push_back(paragraph("(" + parts.args + ")")
                | color(acecode::tui::tool_call_argument_color(palette))
                | flex);
        }
    }
    auto line = hbox(std::move(segs));
    if (focused_message) {
        line = line | focus;
    }
    return line;
}
ftxui::Element render_tool_result_row(const TuiState::Message& msg,
    const ftxui::Box& chat_box, bool transcript_expanded, bool focused_message) {
    // 新优先级:有结构化 hunks → 走彩色 diff 视图(summary + 色带);
    // 其次 summary(无 hunks)→ 单行摘要;都没有 → 灰色 fold。
    // row_expanded = 逐行 Ctrl+E 或 全局 Ctrl+O(transcript_expanded)。
    const bool row_expanded = msg.expanded || transcript_expanded;
    const bool use_diff = msg.hunks.has_value();
    const bool use_summary = msg.summary.has_value() && !row_expanded && !use_diff;
    if (use_diff) {
        // ---- Diff 视图:summary 行 + 彩色 diff 块 ----
        Elements rows;
        if (msg.summary.has_value()) {
            const auto& s = *msg.summary;
            const Color row_color =
                acecode::tui::tool_result_text_color(tui::theme());
            std::string metric_str;
            for (const auto& kv : s.metrics) {
                std::string seg;
                if (kv.first == "+") seg = "+" + kv.second;
                else if (kv.first == "-") seg = "-" + kv.second;
                else seg = kv.first + "=" + kv.second;
                if (!metric_str.empty()) metric_str += " \xC2\xB7 ";
                metric_str += seg;
            }
            const int summary_width = std::max(
                20, chat_box.x_max - chat_box.x_min - 4);
            std::string summary_line = tui::renderable_tool_summary_line(
                s, metric_str, summary_width);
            rows.push_back(hbox({
                text("  \xE2\x94\x94 ") | color(tui::theme().ui.text_dim), // "└"
                text(summary_line) | color(row_color) | dim | flex,
            }));
        } else {
            rows.push_back(hbox({
                text("  \xE2\x94\x94 ") | color(tui::theme().ui.text_dim), // "└"
                text("diff") |
                    color(acecode::tui::tool_result_text_color(tui::theme())) |
                    dim | flex,
            }));
        }

        // 失败态:把前 3 行 stderr dim 显示在 summary 之下(保留既有行为)。
        if (msg.summary.has_value() && !tui::is_success_summary(*msg.summary) &&
            !msg.content.empty()) {
            int shown = 0;
            size_t pos = 0;
            while (pos < msg.content.size() && shown < 3) {
                size_t nl = msg.content.find('\n', pos);
                std::string line = (nl == std::string::npos)
                    ? msg.content.substr(pos)
                    : msg.content.substr(pos, nl - pos);
                rows.push_back(hbox({
                    text("    ") | color(tui::theme().ui.text_dim),
                    paragraph(line) | color(tui::theme().ui.text_muted) | dim | flex,
                }));
                if (nl == std::string::npos) break;
                pos = nl + 1;
                ++shown;
            }
        }

        // Diff 视图:缩进 4 列(与 "  └ " 前缀同宽),宽度由 chat_box 推导。
        DiffViewOptions opts;
        opts.width = std::max(20, chat_box.x_max - chat_box.x_min - 4);
        opts.expanded = row_expanded;
        opts.max_hunks = 3;
        opts.max_lines_per_hunk = 20;
        Element diff_el = render_diff_view(*msg.hunks, opts);
        rows.push_back(hbox({
            text("    ") | color(tui::theme().ui.text_dim),
            diff_el | flex,
        }));

        auto block = vbox(std::move(rows));
        if (focused_message) {
            block = block | focus;
        }
        return block;
    } else if (use_summary) {
        // ---- Summary row: single line, icon + verb + object + metrics ----
        const auto& s = *msg.summary;
        const Color row_color =
            acecode::tui::tool_result_text_color(tui::theme());

        // Build metric tail: " · k=v · k=v" but drop k for
        // "time"/"bytes"/"lines"/"size" since the value is self-describing.
        std::string metric_str;
        for (const auto& kv : s.metrics) {
            std::string seg;
            if (kv.first == "time" || kv.first == "bytes" ||
                kv.first == "size" || kv.first == "lines") {
                seg = kv.second + (kv.first == "lines" ? " lines" : "");
            } else if (kv.first == "+") {
                seg = "+" + kv.second;
            } else if (kv.first == "-") {
                seg = "-" + kv.second;
            } else if (kv.first == "exit") {
                seg = "exit " + kv.second;
            } else if (kv.first == "truncated" && kv.second == "true") {
                seg = "truncated";
            } else if (kv.first == "aborted" && kv.second == "true") {
                seg = "aborted";
            } else if (kv.first == "timeout" && kv.second == "true") {
                seg = "timeout";
            } else if (kv.first == "hint") {
                seg = "hint:" + kv.second;
            } else {
                seg = kv.first + "=" + kv.second;
            }
            if (!metric_str.empty()) metric_str += " \xC2\xB7 "; // " · "
            metric_str += seg;
        }

        const int summary_width = std::max(
            20, chat_box.x_max - chat_box.x_min - 4);
        std::string summary_line = tui::renderable_tool_summary_line(
            s, metric_str, summary_width);

        Elements rows;
        rows.push_back(hbox({
            text("  \xE2\x94\x94 ") | color(tui::theme().ui.text_dim), // "└"
            text(summary_line) | color(row_color) | dim | flex,
        }));

        // Failed tool: render the first 3 lines of output dimmed
        // below the summary so the error is visible without expand.
        if (!tui::is_success_summary(s) && !msg.content.empty()) {
            int shown = 0;
            size_t pos = 0;
            while (pos < msg.content.size() && shown < 3) {
                size_t nl = msg.content.find('\n', pos);
                std::string line = (nl == std::string::npos)
                    ? msg.content.substr(pos)
                    : msg.content.substr(pos, nl - pos);
                rows.push_back(hbox({
                    text("    ") | color(tui::theme().ui.text_dim),
                    paragraph(line) | color(tui::theme().ui.text_muted) | dim | flex,
                }));
                if (nl == std::string::npos) break;
                pos = nl + 1;
                ++shown;
            }
        }

        auto block = vbox(std::move(rows));
        if (focused_message) {
            block = block | focus;
        }
        return block;
    } else {
        // ---- Legacy fold path(含 Ctrl+E/Ctrl+O 展开后的全文视图)----
        // 折叠态按终端可视行而不只是硬换行计数。MCP 等工具常返回
        // 含字面量 "\\n" 的单行 JSON;若只数 '\n',paragraph() 的软
        // 换行仍会铺满屏幕。展开态保留 2000 个硬行的兜底上限。
        std::string display_content;
        if (!row_expanded) {
            constexpr std::size_t kMaxPreviewRows = 3;
            const int content_width = std::max(
                20, chat_box.x_max - chat_box.x_min - 4);
            const auto preview = acecode::tui::fold_tool_result_preview(
                msg.content, content_width, kMaxPreviewRows);
            for (std::size_t line_index = 0;
                 line_index < preview.lines.size(); ++line_index) {
                if (line_index > 0) display_content.push_back('\n');
                display_content += preview.lines[line_index];
            }
            if (preview.folded) {
                if (!display_content.empty()) display_content.push_back('\n');
                display_content += "\xE2\x80\xA6 folded (ctrl+o)";
            }
        } else {
            constexpr int kMaxExpandedHardLines = 2000;
            display_content = msg.content;
            int line_count = 0;
            for (char c : msg.content) if (c == '\n') line_count++;
            if (msg.content.empty() || msg.content.back() != '\n') line_count++;

            if (line_count > kMaxExpandedHardLines) {
                size_t cut = 0;
                int seen = 0;
                while (cut < msg.content.size() &&
                       seen < kMaxExpandedHardLines) {
                    if (msg.content[cut] == '\n') seen++;
                    cut++;
                }
                display_content = msg.content.substr(0, cut);
                if (!display_content.empty() && display_content.back() == '\n') {
                    display_content.pop_back();
                }
                const int hidden = line_count - kMaxExpandedHardLines;
                display_content += "\n\xE2\x80\xA6 +" +
                    std::to_string(hidden) + " lines";
            }
        }

        auto line = hbox({
            text("  \xE2\x94\x94 ") | color(tui::theme().ui.text_dim), // "└"
            tui::render_tool_result_lines_preserving_breaks(display_content) | flex,
        });
        if (focused_message) {
            line = line | focus;
        }
        return line;
    }
}

} // namespace acecode::tui
