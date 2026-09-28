#include "tui/render/tool_row_view.hpp"
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

using namespace ftxui;

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


} // namespace acecode::tui
