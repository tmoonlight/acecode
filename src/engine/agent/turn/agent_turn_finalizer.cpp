#include "agent/agent_loop.hpp"
#include "turn_finalizer.hpp"

namespace acecode {
std::unique_ptr<agent::TurnFinalizer> AgentLoop::make_turn_finalizer() {
    return std::make_unique<agent::TurnFinalizer>(agent::TurnFinalizerServices{
        *history_, *transcript_, *turn_outcome_, *active_turn_gate_, *goal_, *hooks_,
        *activity_, events_, callbacks_, tools_, tool_capability_policy(), busy_,
        abort_signal_, turn_interrupt_requested_, session_manager_});
}
} // namespace acecode
