#pragma once
#include "tui/input/ports.hpp"
#include "tui/tui_state.hpp"
#include "tui/ask_question_adapter.hpp"

namespace acecode::tui {
struct TuiInputContext;
InputDisposition handle_ask_question_input(TuiState& state, IScreenPort& screen,
    const ftxui::Event& event, AskQuestionFrame& ask_question_frame);
InputDisposition handle_ask_question_input(TuiInputContext& context, const ftxui::Event& event);

}
