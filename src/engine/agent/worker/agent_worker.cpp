#include "agent/turn/turn_finalizer.hpp"
#include "agent/transcript/trajectory_recorder.hpp"
#include "agent/turn/turn_context.hpp"
#include "agent/agent_loop.hpp"
#include "agent/model_step/turn_usage_accountant.hpp"
#include "agent/hook_bridge/agent_hook_bridge.hpp"
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
        try {
            loop_cfg_ = *std::atomic_load(&published_loop_config_);
            if (task.kind == WorkerTask::Kind::Chat) {
                turn_context_ = std::make_unique<agent::TurnContext>(callbacks_.snapshot());
            }
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
            if (turn_context_) turn_context_->desktop_lease.reset();
            recover_worker_task_error(error.what(), task.kind == WorkerTask::Kind::Chat);
        } catch (...) {
            if (turn_context_) turn_context_->desktop_lease.reset();
            recover_worker_task_error("unknown exception", task.kind == WorkerTask::Kind::Chat);
        }
        turn_context_.reset();
        task_queue_->finish_task();
    }
}

void AgentLoop::recover_worker_task_error(const char* detail, bool chat_task) {
    make_turn_finalizer()->recover(
        turn_context_.get(), detail, chat_task,
        trajectory_ ? trajectory_->ref() : LifetimeRef<agent::TrajectoryRecorder>{});

}

} // namespace acecode
