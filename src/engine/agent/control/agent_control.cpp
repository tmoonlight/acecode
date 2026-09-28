#include "agent/agent_loop.hpp"
#include "agent/control/task_handoff.hpp"
#include "agent/worker/agent_task_queue.hpp"
#include "agent/transcript/transcript_queries.hpp"
#include "session/session_manager.hpp"
#include "session/system_notice.hpp"
#include <utility>

namespace acecode {

using agent::detail::trailing_transcript_message;

void AgentLoop::submit(const std::string& user_message) {
    submit(user_message, std::string{});
}

void AgentLoop::submit(const std::string& prompt, const std::string& display_text) {
    UserInput input;
    input.text = prompt;
    input.display_text = display_text;
    submit(input);
}

void AgentLoop::submit(const UserInput& input) {
    clear_stale_abort_request();
    WorkerTask task;
    task.kind = WorkerTask::Kind::Chat;
    task.input = input;
    task.hidden_goal_context = false;
    task_queue_->enqueue(std::move(task));
}

std::optional<ChatMessage> AgentLoop::retryable_user_message(
    const std::string& expected_user_message_id) const {
    if (expected_user_message_id.empty() || live_transcript_tail_blocked_.load()) return std::nullopt;
    const auto* model_tail = trailing_transcript_message(messages_);
    if (!model_tail || (model_tail->role != "user" &&
        model_tail->role != "assistant" && model_tail->role != "tool")) return std::nullopt;
    bool user_aborted = false;
    if (session_manager_) {
        const auto persisted = session_manager_->load_active_messages();
        const auto* tail = trailing_transcript_message(persisted);
        user_aborted = tail && tail->role == "system" && tail->metadata.is_object() &&
            tail->metadata.value("transcript_only", false) &&
            tail->metadata.value("user_aborted", false) &&
            tail->metadata.value("retry_user_message_id", std::string{}) == expected_user_message_id;
        if (user_aborted) tail = trailing_transcript_message(persisted, true);
        if (!tail || tail->role != "user" ||
            tail->uuid != expected_user_message_id) return std::nullopt;
    }
    const auto* message = trailing_transcript_message(messages_, user_aborted);
    if (!message || message->role != "user" ||
        message->uuid != expected_user_message_id) return std::nullopt;
    return *message;
}

bool AgentLoop::retry_last_user_message(
    const std::string& expected_user_message_id, std::string& error) {
    const bool accepted = task_queue_->with_locked([&](agent::AgentTaskQueue::Locked& queue) {
        if (!queue.idle()) {
            error = "session has active or queued work";
            return false;
        }
        if (!retryable_user_message(expected_user_message_id)) {
            error = "user message is not eligible for retry";
            return false;
        }
        WorkerTask task;
        task.kind = WorkerTask::Kind::Chat;
        task.retry_user_message_id = expected_user_message_id;
        queue.push(std::move(task));
        abort_signal_.clear();
        return true;
    });
    if (!accepted) return false;
    error.clear();
    task_queue_->notify();
    return true;
}

ControlEnqueueReceipt AgentLoop::enqueue_control(std::function<bool()> control) {
    return task_queue_->enqueue_control(std::move(control));
}

bool AgentLoop::try_run_idle_control(const std::function<void()>& control) {
    return task_queue_->try_run_idle(control);
}

void AgentLoop::submit_shell(std::string command) {
    clear_stale_abort_request();
    task_queue_->enqueue(WorkerTask{WorkerTask::Kind::Shell, std::move(command)});
}

void AgentLoop::submit_compact() {
    clear_stale_abort_request();
    WorkerTask task;
    task.kind = WorkerTask::Kind::Compact;
    task_queue_->enqueue(std::move(task));
}


bool AgentLoop::submit_task_suggestion_input(const UserInput& input,
                                             const std::string& suggestion_id) {
    return task_queue_->enqueue_suggestion(input, suggestion_id, abort_signal_);
}

bool AgentLoop::try_start_side_task(
    const std::function<bool()>& accept_target_input, std::string* error) {
    return task_handoff_->try_start_side_task(accept_target_input, error);
}

bool AgentLoop::complete_task_handoff(
    const std::string& target_session_id,
    const std::function<bool()>& accept_target_input, std::string* error) {
    const auto result = task_handoff_->complete(
        session_manager_, target_session_id, accept_target_input, error);
    if (!result.accepted) return false;
    if (result.paused_goal) emit_goal_updated(*result.paused_goal);
    emit_transcript_system_message(
        "Continued in session " + target_session_id + ".",
        make_system_notice_metadata("session_continued", {{"session", target_session_id}},
            {{"task_handoff", true}, {"target_session_id", target_session_id}}));
    return true;
}

} // namespace acecode
