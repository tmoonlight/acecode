#include "agent/agent_loop.hpp"
#include "agent/detail/agent_payloads.hpp"
#include "agent/guards/doom_guard.hpp"
#include "pa/pa_context_budget.hpp"
#include "permissions/shell_write_guard.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_client.hpp"
#include "session/session_storage.hpp"
#include "session/token_tracker.hpp"
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

using agent::detail::model_step_usage_to_json;

void AgentLoop::worker_main() {
    while (true) {
        WorkerTask task;
        {
            std::unique_lock<std::mutex> lk(queue_mu_);
            queue_cv_.wait(lk, [this] {
                return !priority_task_queue_.empty() ||
                       !task_queue_.empty() || shutdown_requested_;
            });
            if (shutdown_requested_) return;
            if (!priority_task_queue_.empty()) {
                task = std::move(priority_task_queue_.front());
                priority_task_queue_.pop();
            } else {
                task = std::move(task_queue_.front());
                task_queue_.pop();
            }
            worker_task_active_ = true;
            worker_task_kind_ = task.kind;
        }
        if (task.kind == WorkerTask::Kind::Chat) {
            active_turn_usage_ = TokenUsage{};
            active_turn_usage_initialized_ = false;
        }
        try {
            switch (task.kind) {
            case WorkerTask::Kind::Chat:
                if (!task.retry_user_message_id.empty()) {
                    const auto message = retryable_user_message(task.retry_user_message_id);
                    if (!message) {
                        throw std::runtime_error("user message is no longer eligible for retry");
                    }
                    UserInput input;
                    input.text = message->content;
                    input.content_parts = message->content_parts;
                    input.metadata = message->metadata;
                    if (message->metadata.is_object()) {
                        input.display_text = message->metadata.value("display_text", std::string{});
                    }
                    run_agent_with_input(input, false, &*message);
                    break;
                }
                if (task.input.empty() && !task.payload.empty()) {
                    task.input.text = std::move(task.payload);
                    task.input.display_text = std::move(task.display_text);
                }
                run_agent_with_input(task.input, task.hidden_goal_context);
                break;
            case WorkerTask::Kind::Shell:
                run_shell(task.payload);
                break;
            case WorkerTask::Kind::Compact:
                run_compact();
                break;
            case WorkerTask::Kind::Control:
                if (task.control) task.control();
                break;
            }
        } catch (const std::exception& error) {
            recover_worker_task_error(error.what(), task.kind == WorkerTask::Kind::Chat);
        } catch (...) {
            recover_worker_task_error("unknown exception", task.kind == WorkerTask::Kind::Chat);
        }
        {
            std::lock_guard<std::mutex> lk(queue_mu_);
            worker_task_active_ = false;
            worker_task_kind_ = WorkerTask::Kind::Control;
        }
    }
}

void AgentLoop::recover_worker_task_error(const char* detail, bool chat_task) {
    const std::string message = "[Error] Task failed: " + ensure_utf8(detail);
    LOG_ERROR(message);
    const std::string turn_id = active_turn_id();
    close_active_turn_and_discard();
    turn_interrupt_requested_ = false;
    active_turn_swarm_mode_ = false;
    hook_request_context_.clear();
    {
        std::lock_guard<std::mutex> lock(active_provider_mu_);
        active_provider_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(last_turn_error_mu_);
        last_turn_error_ = message;
    }
    record_turn_outcome("error");
    busy_ = false;

    // Reporting may itself call the callback that threw. Isolate each step so
    // a broken consumer cannot suppress terminal events or kill the worker.
    auto attempt = [](const auto& report) {
        try {
            report();
        } catch (const std::exception& error) {
            LOG_ERROR(std::string("Task error reporting failed: ") + error.what());
        } catch (...) {
            LOG_ERROR("Task error reporting failed with unknown exception");
        }
    };
    attempt([&] { stop_active_goal_after_turn_error(ProviderErrorInfo{}); });
    attempt([&] { dispatch_message("error", message, false); });
    attempt([&] {
        if (chat_task && callbacks_.on_turn_finished) callbacks_.on_turn_finished("error");
    });
    nlohmann::json idle = {
        {"busy", false}, {"outcome", "error"}, {"turn_id", turn_id}};
    nlohmann::json done = {{"outcome", "error"}};
    if (chat_task) {
        const auto usage = model_step_usage_to_json(active_turn_usage_);
        idle["usage"] = usage;
        done["turn_id"] = turn_id;
        done["usage"] = usage;
    }
    attempt([&] { record_terminal_trajectory_events(idle, done); });
    attempt([&] {
        if (callbacks_.on_busy_changed) callbacks_.on_busy_changed(false);
    });
    attempt([&] { events_.emit(SessionEventKind::BusyChanged, idle); });
    attempt([&] { events_.emit(SessionEventKind::Done, done); });
}

} // namespace acecode
