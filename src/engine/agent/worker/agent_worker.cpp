#include "agent/agent_loop.hpp"
#include "agent/turn/turn_outcome.hpp"
#include "agent/worker/agent_task_queue.hpp"
#include "agent/detail/agent_payloads.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include <stdexcept>
#include <utility>

namespace acecode {

using agent::detail::model_step_usage_to_json;

void AgentLoop::worker_main() {
    while (true) {
        WorkerTask task;
        if (!task_queue_->wait_pop(task)) return;
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
        task_queue_->finish_task();
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
    turn_outcome_->set_error(message);
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
