#pragma once
#include "tui/input/tui_input_context.hpp"

namespace acecode::tui {
void insert_pasted_text_at_cursor_locked(TuiState& state, const std::string& normalized);
bool can_accept_clipboard_paste_locked(const TuiState& state);
bool paste_clipboard_text(TuiInputContext& ctx);
bool paste_clipboard_image(TuiInputContext& ctx);
InputDisposition handle_bracketed_paste(TuiInputContext& ctx, const ftxui::Event& event);
}
