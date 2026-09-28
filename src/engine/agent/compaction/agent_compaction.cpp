#include "agent/turn/turn_context.hpp"
#include "agent/agent_loop.hpp"
#include "compaction_controller.hpp"
#include "agent/transcript/trajectory_recorder.hpp"

namespace acecode {
agent::CompactionInputs AgentLoop::compaction_inputs() const {
    agent::CompactionInputs inputs;
    inputs.session = session_manager_;
    inputs.hooks = hook_manager_;
    inputs.provider = provider_accessor_ ? provider_accessor_() : nullptr;
    inputs.request = request_context_options(inputs.provider, turn_context_ && turn_context_->swarm_mode);
    inputs.suggestion_threshold = task_suggestion_compact_threshold_.load(std::memory_order_relaxed);
    if (trajectory_) inputs.terminal = trajectory_->ref();
    return inputs;
}
bool AgentLoop::maybe_run_auto_compact() {
    return compaction_->run_auto(compaction_inputs());
}
bool AgentLoop::active_estimate_exceeds_auto_threshold(const UserInput* pending) const {
    return compaction_->exceeds_auto_threshold(compaction_inputs(), pending);
}
void AgentLoop::run_compact() {
    compaction_->run_manual(compaction_inputs());
}
} // namespace acecode
