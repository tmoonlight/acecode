#include "agent/agent_loop.hpp"
#include "agent/goal/goal_runtime.hpp"

namespace acecode {

void AgentLoop::restore_goal_runtime() { goal_->restore(session_manager_); }
void AgentLoop::publish_current_goal_state() { goal_->publish(session_manager_); }
void AgentLoop::emit_goal_updated(const ThreadGoal& goal) { goal_->emit_updated(goal); }
void AgentLoop::maybe_continue_goal() {
    goal_->maybe_continue(session_manager_, {
        tools_.is_allowed("update_goal", &request_source_.tool_policy),
        tools_.is_allowed("AskUserQuestion", &request_source_.tool_policy)});
}
bool AgentLoop::goal_unattended_active() { return goal_->unattended_active(session_manager_); }
void AgentLoop::notify_goal_objective_updated() { goal_->notify_objective_updated(); }

} // namespace acecode
