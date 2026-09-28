#include "agent/agent_loop.hpp"
#include "agent/transcript/trajectory_recorder.hpp"
#include "turn_runner.hpp"
#include "turn_context.hpp"

namespace acecode {
void AgentLoop::run_agent_with_input(const UserInput& input, bool hidden_goal_context,
                                    const ChatMessage* retry_message) {
    const auto& source = turn_context_->request_source;
    auto runner = std::make_unique<agent::TurnRunner>(
        agent::TurnRunnerServices{
            agent::ToolExecutionServices{tools_, callbacks_, permissions_, *history_,
                *transcript_, *hooks_, *tool_hooks_, *boundary_, *exec_security_, *prompt_cache_,
                *goal_, events_, abort_signal_, loop_cfg_, source.tool_policy,
                session_manager_, hook_manager_, source.skills.get(), prompter_.get(), ask_prompter_.get()},
            *turn_outcome_, *active_turn_gate_, *activity_, *side_questions_, *usage_accountant_,
            *model_steps_, *stream_collector_, *compaction_, *recovery_, *request_builder_,
            source, busy_, turn_interrupt_requested_, context_window_,
            task_suggestion_compact_threshold_},
        agent::TurnRunnerOptions{provider_accessor_, ask_channel_, progress_clock_,
            computer_use_release_, no_model_config_prompt_});
    runner->run(*turn_context_, input, hidden_goal_context, retry_message,
        trajectory_ ? trajectory_->ref() : LifetimeRef<agent::TrajectoryRecorder>{});
}
} // namespace acecode
