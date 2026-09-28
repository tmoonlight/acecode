#pragma once
#include "tui/input/ports.hpp"
#include "tui/tui_state.hpp"
#include "tui/ask_question_adapter.hpp"

namespace acecode::tui {
InputDisposition handle_ask_question_input(TuiState& state, IScreenPort& screen,
    ftxui::Event& event, AskQuestionFrame& ask_question_frame);
}
