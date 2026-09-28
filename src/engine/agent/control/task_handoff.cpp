#include "agent/agent_loop.hpp"
#include "hooks/hook_runtime.hpp"
#include "permissions/shell_write_guard.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "session/session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/system_notice.hpp"
#include "session/thread_goal_store.hpp"
#include "session/token_tracker.hpp"
#include "session/turn_timing.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode {

bool AgentLoop::submit_task_suggestion_input(const UserInput& input,
                                            const std::string& suggestion_id) {
    if (suggestion_id.empty() || input.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        if (shutdown_requested_) return false;
        if (task_suggestion_input_ids_.count(suggestion_id)) return true;
        // An unrelated turn must not be mistaken for this suggestion's
        // receipt, including input queued before busy becomes true.
        if (busy_.load() || has_queued_user_work_locked() ||
            (worker_task_active_ && worker_task_kind_ != WorkerTask::Kind::Control)) return false;
        WorkerTask task;
        task.kind = WorkerTask::Kind::Chat;
        task.input = input;
        if (!task.input.metadata.is_object()) task.input.metadata = nlohmann::json::object();
        task.input.metadata["task_suggestion_id"] = suggestion_id;
        task.hidden_goal_context = false;
        task_queue_.push(std::move(task));
        task_suggestion_input_ids_.insert(suggestion_id);
        abort_requested_ = false;
    }
    queue_cv_.notify_one();
    return true;
}

bool AgentLoop::try_start_side_task(
    const std::function<bool()>& accept_target_input, std::string* error) {
    if (error) error->clear();
    std::lock_guard<std::mutex> lock(queue_mu_);
    if (shutdown_requested_ || busy_.load() ||
        (worker_task_active_ && worker_task_kind_ != WorkerTask::Kind::Control) ||
        !task_queue_.empty() || !priority_task_queue_.empty()) {
        if (error) *error = "source session has pending work";
        return false;
    }
    if (!accept_target_input || !accept_target_input()) {
        if (error) *error = "target input was not accepted";
        return false;
    }
    return true;
}

bool AgentLoop::complete_task_handoff(
    const std::string& target_session_id,
    const std::function<bool()>& accept_target_input,
    std::string* error) {
    if (error) error->clear();
    auto fail = [error](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (target_session_id.empty() || !session_manager_ || !accept_target_input) {
        return fail("handoff requires a source session, target and input callback");
    }
    std::optional<ThreadGoal> paused_goal;
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        if (shutdown_requested_ || busy_.load() ||
            (worker_task_active_ && worker_task_kind_ != WorkerTask::Kind::Control)) {
            return fail("source session is still running");
        }
        if (has_queued_user_work_locked()) {
            return fail("source session has pending user input");
        }
        const auto source = session_manager_->current_session_id();
        if (source.empty() || source == target_session_id) return fail("invalid handoff target");
        auto* goals = session_manager_->existing_goal_store();
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
        auto remove_goal_continuations = [](std::queue<WorkerTask>& queue) {
            std::queue<WorkerTask> retained;
            while (!queue.empty()) {
                auto task = std::move(queue.front());
                queue.pop();
                if (task.kind == WorkerTask::Kind::Chat && task.hidden_goal_context) continue;
                retained.push(std::move(task));
            }
            queue.swap(retained);
        };
        remove_goal_continuations(task_queue_);
        remove_goal_continuations(priority_task_queue_);
    }
    if (paused_goal) emit_goal_updated(*paused_goal);
    emit_transcript_system_message(
        "Continued in session " + target_session_id + ".",
        make_system_notice_metadata("session_continued", {{"session", target_session_id}},
            {{"task_handoff", true}, {"target_session_id", target_session_id}}));
    return true;
}

} // namespace acecode
