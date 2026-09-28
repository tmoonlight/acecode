#pragma once
#include "tui/tui_state.hpp"
#include <ftxui/dom/elements.hpp>

namespace acecode::tui {
ftxui::Element render_user_message_row(const TuiState::Message& msg, bool focused_message);
ftxui::Element render_assistant_message_row(ftxui::Element md_content, bool focused_message);
ftxui::Element render_tool_text_row(ftxui::Element content, bool focused_message);
// Null for unknown roles, matching the original omitted-row behavior.
ftxui::Element render_notice_message_row(const TuiState::Message& msg,
    bool transcript_expanded, bool focused_message);
}
