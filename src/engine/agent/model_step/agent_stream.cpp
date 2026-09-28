#include "agent/agent_loop.hpp"
#include "provider_stream_collector.hpp"
#include "turn_usage_accountant.hpp"

namespace acecode {
AgentLoop::ProviderCallResult AgentLoop::call_provider_and_collect(
    const std::shared_ptr<LlmProvider>& provider, const ApiRequestBundle& bundle,
    const ProgressEmitter& emit_progress, int model_step_index) {
    return stream_collector_->collect(provider, bundle, emit_progress, model_step_index,
                                      *turn_usage_, session_manager_);
}
} // namespace acecode
