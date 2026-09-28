#pragma once
#include "tui/tui_state.hpp"
namespace acecode::tui {
enum class UserTurnPhrase { Random, Shell };
enum class WaitingUpdate { SetTrue, Preserve };
// Caller holds state.mu. Transcript, follow-tail, observation, input, abort,
// and tool-progress fields deliberately remain the responsibility of callers.
void begin_user_turn_locked(TuiState& state,
    UserTurnPhrase phrase = UserTurnPhrase::Random,
    WaitingUpdate waiting = WaitingUpdate::SetTrue);
}
