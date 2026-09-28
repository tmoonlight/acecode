#include "agent/agent_loop.hpp"
#include "tool_batch_scheduler.hpp"

namespace acecode {
agent::ToolBatchOutcome AgentLoop::execute_tool_calls(
    const ChatResponse& response, const std::shared_ptr<LlmProvider>& provider,
    const ProgressEmitter& progress, agent::SynchronizedDoomGuard& doom_guard,
    ToolPreambleTitle& preamble) {
    auto pipeline = std::make_unique<agent::ToolBatchScheduler>(
        agent::ToolExecutionServices{
            tools_, callbacks_, permissions_, *history_, *transcript_, *hooks_,
            *tool_hooks_, *boundary_, *exec_security_, *prompt_cache_, *goal_,
            events_, abort_signal_, loop_cfg_, tool_capability_policy_,
            session_manager_, hook_manager_, skill_registry_, prompter_.get(), ask_prompter_},
        agent::ToolExecutionOptions{
            provider_accessor_, ask_channel_, turn_progress_clock_, current_request_model_tool_names_});
    return pipeline->execute(response, provider, progress, doom_guard, preamble);
}
} // namespace acecode
