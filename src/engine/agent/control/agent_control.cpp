#include "agent/agent_loop.hpp"
#include "agent/transcript/transcript_queries.hpp"
#include "llm/tool_protocol_names.hpp"
#include "permissions/interaction_mode.hpp"
#include "permissions/shell_write_guard.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
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
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        WorkerTask task;
        task.kind = WorkerTask::Kind::Chat;
        task.input = input;
        task.hidden_goal_context = false;
        task_queue_.push(std::move(task));
    }
    queue_cv_.notify_one();
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
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        if (shutdown_requested_ || worker_task_active_ || busy_.load() ||
            !priority_task_queue_.empty() || !task_queue_.empty()) {
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
        task_queue_.push(std::move(task));
        abort_requested_ = false;
    }
    error.clear();
    queue_cv_.notify_one();
    return true;
}

ControlEnqueueReceipt AgentLoop::enqueue_control(
    std::function<bool()> control) {
    ControlEnqueueReceipt receipt;
    if (!control) return receipt;
    auto execution = std::make_shared<ControlExecutionState>();
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        if (shutdown_requested_) return receipt;

        auto is_turn_task = [](WorkerTask::Kind kind) {
            return kind == WorkerTask::Kind::Chat ||
                   kind == WorkerTask::Kind::Shell ||
                   kind == WorkerTask::Kind::Compact;
        };
        bool queued_behind_turn =
            worker_task_active_ && is_turn_task(worker_task_kind_);
        auto urgent = priority_task_queue_;
        while (!queued_behind_turn && !urgent.empty()) {
            queued_behind_turn = is_turn_task(urgent.front().kind);
            urgent.pop();
        }
        auto ordinary = task_queue_;
        while (!queued_behind_turn && !ordinary.empty()) {
            queued_behind_turn = is_turn_task(ordinary.front().kind);
            ordinary.pop();
        }

        WorkerTask task;
        task.kind = WorkerTask::Kind::Control;
        task.control = [control = std::move(control), execution]() mutable {
            bool succeeded = false;
            try {
                succeeded = control();
            } catch (const std::exception& e) {
                LOG_ERROR(std::string("Control task failed: ") + e.what());
            } catch (...) {
                LOG_ERROR("Control task failed with unknown exception");
            }
            {
                std::lock_guard<std::mutex> lock(execution->mu);
                execution->succeeded = succeeded;
                execution->completed = true;
            }
            execution->cv.notify_all();
        };
        task_queue_.push(std::move(task));
        receipt.sequence = ++next_control_sequence_;
        receipt.accepted = true;
        receipt.queued_behind_turn = queued_behind_turn;
        receipt.execution = std::move(execution);
    }
    queue_cv_.notify_one();
    return receipt;
}

bool AgentLoop::try_run_idle_control(const std::function<void()>& control) {
    if (!control) return false;
    std::lock_guard<std::mutex> lock(queue_mu_);
    if (shutdown_requested_ || worker_task_active_ || busy_.load() ||
        !priority_task_queue_.empty() || !task_queue_.empty()) {
        return false;
    }
    control();
    return true;
}

void AgentLoop::submit_shell(std::string command) {
    clear_stale_abort_request();
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        task_queue_.push(WorkerTask{WorkerTask::Kind::Shell, std::move(command)});
    }
    queue_cv_.notify_one();
}

void AgentLoop::submit_compact() {
    clear_stale_abort_request();
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        WorkerTask task;
        task.kind = WorkerTask::Kind::Compact;
        task_queue_.push(std::move(task));
    }
    queue_cv_.notify_one();
}

} // namespace acecode
