#include "agent/agent_loop.hpp"
#include "context_overflow_recovery.hpp"

namespace acecode {
AgentLoop::HandleErrorResult AgentLoop::handle_provider_error(
    ProviderCallResult& result, const std::vector<ChatMessage>& messages,
    std::string& timing_status) {
    const auto outcome = recovery_->resolve(result, messages, *recovery_state_,
                                            context_window(), session_manager_);
    if (outcome.timing_status) timing_status = *outcome.timing_status;
    return outcome.decision;
}
} // namespace acecode
