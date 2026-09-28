#pragma once
#include "tui/tui_state.hpp"
#include "tui/tool_row_format.hpp"
#include <ftxui/dom/elements.hpp>

namespace acecode::tui {
bool is_success_summary(const ToolSummary& summary);
std::string renderable_tool_summary_line(const ToolSummary& summary,
    const std::string& metric_str, int max_visual_width);
ftxui::Element render_tool_result_lines_preserving_breaks(const std::string& display_content);
ftxui::Element render_tool_call_row(const TuiState::Message& msg, ToolCallDot tool_dot,
    bool transcript_expanded, bool focused_message);
ftxui::Element render_tool_result_row(const TuiState::Message& msg,
    const ftxui::Box& chat_box, bool transcript_expanded, bool focused_message);

} // namespace acecode::tui
