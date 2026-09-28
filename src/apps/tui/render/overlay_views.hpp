#pragma once
#include "tui/tui_state.hpp"
#include "tui/ask_question_adapter.hpp"
#include <ftxui/dom/elements.hpp>

namespace acecode::tui {
struct OverlayViews { ftxui::Element ask, confirm; };
// Caller holds state.mu; question layout may project its scroll correction.
OverlayViews render_overlay_views(TuiState& state, AskQuestionFrame& ask_question_frame,
    int terminal_width, int current_message_width, bool show_regular_sidebar, int viewport_rows);
}
