#include "agent/agent_loop.hpp"
#include "agent/goal/goal_runtime.hpp"

namespace acecode {

void AgentLoop::restore_goal_runtime() { goal_->restore(session_manager_); }
void AgentLoop::publish_current_goal_state() { goal_->publish(session_manager_); }
void AgentLoop::emit_goal_updated(const ThreadGoal& goal) { goal_->emit_updated(goal); }
void AgentLoop::emit_goal_cleared(const std::string& id) { goal_->emit_cleared(id); }
void AgentLoop::emit_todo_updated(const nlohmann::json& payload) { goal_->emit_todo_updated(session_manager_, payload); }
void AgentLoop::account_goal_usage(std::int64_t delta, bool allow_complete) {
    goal_->account_usage(session_manager_, delta, allow_complete);
}
void AgentLoop::maybe_continue_goal() {
    goal_->maybe_continue(session_manager_, {
        tools_.is_allowed("update_goal", &request_source_.tool_policy),
        tools_.is_allowed("AskUserQuestion", &request_source_.tool_policy)});
}
bool AgentLoop::goal_unattended_active() { return goal_->unattended_active(session_manager_); }
void AgentLoop::notify_goal_objective_updated() { goal_->notify_objective_updated(); }
void AgentLoop::stop_active_goal_after_turn_error(const ProviderErrorInfo& info) {
    goal_->stop_after_error(session_manager_, info);
}
void AgentLoop::maybe_inject_goal_steering() {
    goal_->inject_steering(session_manager_, {
        tools_.is_allowed("update_goal", &request_source_.tool_policy),
        tools_.is_allowed("AskUserQuestion", &request_source_.tool_policy)});
}

std::string AgentLoop::build_goal_context_prompt(const ThreadGoal& goal) const {
    return agent::detail::build_goal_context_prompt(goal, {
        tools_.is_allowed("update_goal", &request_source_.tool_policy),
        tools_.is_allowed("AskUserQuestion", &request_source_.tool_policy)});
}

std::string AgentLoop::build_goal_budget_limit_prompt(const ThreadGoal& goal) const {
    return agent::detail::build_goal_budget_limit_prompt(goal, {
        tools_.is_allowed("update_goal", &request_source_.tool_policy),
        tools_.is_allowed("AskUserQuestion", &request_source_.tool_policy)});
}

std::string AgentLoop::build_goal_objective_updated_prompt(const ThreadGoal& goal) const {
    return agent::detail::build_goal_objective_updated_prompt(goal, {
        tools_.is_allowed("update_goal", &request_source_.tool_policy),
        tools_.is_allowed("AskUserQuestion", &request_source_.tool_policy)});
}

} // namespace acecode
