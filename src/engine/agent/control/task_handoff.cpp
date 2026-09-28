#include "task_handoff.hpp"

#include "agent/worker/agent_task_queue.hpp"
#include "session/session_manager.hpp"

#include <exception>
#include <utility>

namespace acecode::agent {

bool TaskHandoff::try_start_side_task(
    const std::function<bool()>& accept_target_input, std::string* error) {
    if (error) error->clear();
    return source_.with_locked([&](AgentTaskQueue::Locked& queue) {
        if (queue.stopped() || queue.active_operation() || !queue.empty()) {
            if (error) *error = "source session has pending work";
            return false;
        }
        if (!accept_target_input || !accept_target_input()) {
            if (error) *error = "target input was not accepted";
            return false;
        }
        return true;
    });
}

TaskHandoffResult TaskHandoff::complete(
    SessionManager* session,
    const std::string& target_session_id,
    const std::function<bool()>& accept_target_input,
    std::string* error) {
    if (error) error->clear();
    auto fail = [error](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (target_session_id.empty() || !session || !accept_target_input) {
        fail("handoff requires a source session, target and input callback");
        return {};
    }
    std::optional<ThreadGoal> paused_goal;
    const bool transferred = source_.with_locked([&](AgentTaskQueue::Locked& queue) {
        if (queue.stopped() || queue.active_operation()) {
            return fail("source session is still running");
        }
        if (queue.has_user_work()) {
            return fail("source session has pending user input");
        }
        const auto source = session->current_session_id();
        if (source.empty() || source == target_session_id) return fail("invalid handoff target");
        auto* goals = session->existing_goal_store();
        std::optional<ThreadGoal> original_goal;
        std::string goal_error;
        if (goals) {
            original_goal = goals->get_thread_goal(source, &goal_error);
            if (!goal_error.empty()) return fail(goal_error);
            if (original_goal && original_goal->status == ThreadGoalStatus::Active) {
                if (!goals->pause_active_thread_goal(source, &goal_error)) {
                    return fail(goal_error.empty() ? "could not pause source goal" : goal_error);
                }
                paused_goal = *original_goal;
                paused_goal->status = ThreadGoalStatus::Paused;
            }
        }
        bool accepted = false;
        try {
            accepted = accept_target_input();
        } catch (const std::exception& exception) {
            if (error) *error = exception.what();
        } catch (...) {
            if (error) *error = "target input submission failed";
        }
        if (!accepted) {
            if (paused_goal && goals && !goals->update_thread_goal_status(
                    source, paused_goal->goal_id, ThreadGoalStatus::Active, &goal_error)) {
                return fail("target input failed; could not restore source goal: " + goal_error);
            }
            return fail(error && !error->empty() ? *error : "target input was not accepted");
        }
        queue.remove_goal_continuations();
        return true;
    });
    return {transferred, transferred ? std::move(paused_goal) : std::nullopt};
}

} // namespace acecode::agent
