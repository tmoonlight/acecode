#pragma once
#include "tui/tui_state.hpp"

namespace acecode::tui {
int ask_timeout_remaining_seconds(const AskQuestionSession& session, AskQuestionSession::TimePoint now);
void project_ask_session_locked(TuiState& state);
void dispatch_ask_session_effects_locked(TuiState& state, const std::vector<AskQuestionEffect>& effects);
}
