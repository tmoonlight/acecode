#include "tui/model/user_turn_state.hpp"
#include "tui/model/thinking_phrases.hpp"
namespace acecode::tui {
void begin_user_turn_locked(TuiState& state, UserTurnPhrase phrase, WaitingUpdate waiting) {
    state.current_thinking_phrase = phrase == UserTurnPhrase::Shell
        ? "Running shell" : get_random_thinking_phrase(is_user_chinese(state));
    state.thinking_start_time = std::chrono::steady_clock::now();
    state.streaming_output_chars = 0;
    state.turn_completion_tokens_confirmed = 0;
    if (waiting == WaitingUpdate::SetTrue) state.is_waiting = true;
}
}
