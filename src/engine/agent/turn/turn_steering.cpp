#include "agent/agent_loop.hpp"
#include "agent/turn/user_turn_message.hpp"
#include "hooks/hook_runtime.hpp"
#include "llm/tool_protocol_names.hpp"
#include "pa/pa_overflow_rescue.hpp"
#include "permissions/interaction_mode.hpp"
#include "permissions/shell_write_guard.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_rewind.hpp"
#include "session/session_storage.hpp"
#include "session/thread_goal_store.hpp"
#include "session/thread_repair.hpp"
#include "session/turn_timing.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"
#include "utils/text.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode {

using agent::detail::has_meaningful_user_input;

TurnSteerResult AgentLoop::steer_input(
    const std::string& expected_turn_id,
    const UserInput& input) {
    if (expected_turn_id.empty()) {
        return {
            TurnSteerStatus::InvalidInput,
            {},
            "expected turn id is required",
        };
    }
    if (!has_meaningful_user_input(input)) {
        return {
            TurnSteerStatus::InvalidInput,
            {},
            "steering input is empty",
        };
    }

    std::lock_guard<std::mutex> lk(active_turn_mu_);
    if (!active_turn_accepting_ || active_turn_id_.empty()) {
        return {
            busy_.load()
                ? TurnSteerStatus::NonSteerable
                : TurnSteerStatus::NoActiveTurn,
            {},
            busy_.load()
                ? "the busy operation is not steerable"
                : "no active turn",
        };
    }
    if (expected_turn_id != active_turn_id_) {
        return {
            TurnSteerStatus::TurnMismatch,
            active_turn_id_,
            "expected turn does not match the active turn",
        };
    }
    if (pending_turn_inputs_.size() >= kMaxPendingTurnSteers) {
        return {
            TurnSteerStatus::QueueFull,
            active_turn_id_,
            "active turn steering queue is full",
        };
    }

    pending_turn_inputs_.push_back(input);
    return {
        TurnSteerStatus::Accepted,
        active_turn_id_,
        "accepted",
    };
}

TurnSteerResult AgentLoop::interject_question(
    const std::string& request_id,
    const UserInput& input,
    const std::string& expected_turn_id) {
    if (request_id.empty()) {
        return {
            TurnSteerStatus::InvalidInput,
            {},
            "question request id is required",
        };
    }
    if (!has_meaningful_user_input(input)) {
        return {
            TurnSteerStatus::InvalidInput,
            {},
            "interjection input is empty",
        };
    }
    if (!ask_prompter_) {
        // TUI 走 overlay 通道,提问期间 composer 根本不可达,没有这条路径。
        return {
            TurnSteerStatus::NoPendingQuestion,
            {},
            "this session has no asynchronous question channel",
        };
    }

    // 锁序:只持 active_turn_mu_ 再进 prompter 的锁。prompter 的 prompt()
    // 跑在工具线程上,从不反过来拿 active_turn_mu_;worker 主线程的
    // drain_active_turn_inputs 要拿 active_turn_mu_,但它必须等工具批次
    // 收割完 —— 而收割又要等这里的 notify_response 把问题收掉。所以
    // 在释放锁之前把插话压进 pending_turn_inputs_,排序就是确定的。
    std::lock_guard<std::mutex> lk(active_turn_mu_);
    if (!active_turn_accepting_ || active_turn_id_.empty()) {
        return {
            busy_.load()
                ? TurnSteerStatus::NonSteerable
                : TurnSteerStatus::NoActiveTurn,
            {},
            busy_.load()
                ? "the busy operation is not steerable"
                : "no active turn",
        };
    }
    if (!expected_turn_id.empty() && expected_turn_id != active_turn_id_) {
        return {
            TurnSteerStatus::TurnMismatch,
            active_turn_id_,
            "expected turn does not match the active turn",
        };
    }
    if (pending_turn_inputs_.size() >= kMaxPendingTurnSteers) {
        return {
            TurnSteerStatus::QueueFull,
            active_turn_id_,
            "active turn steering queue is full",
        };
    }

    // 先收问题再压输入:notify_response 是 first-wins,问题已被别的客户端
    // 回答 / 已超时 / 已关闭时返回 false,此时不能把文本静默变成普通 steer
    // —— 调用方拿到 NoPendingQuestion 后自己决定走排队还是直接发送。
    AskUserQuestionResponse response;
    response.cancelled = true;
    response.interjected = true;
    if (!ask_prompter_->notify_response(request_id, response)) {
        return {
            TurnSteerStatus::NoPendingQuestion,
            active_turn_id_,
            "the question is no longer pending",
        };
    }

    UserInput steer = input;
    if (!steer.metadata.is_object()) {
        steer.metadata = nlohmann::json::object();
    }
    steer.metadata["question_interjection"] = true;
    steer.metadata["question_request_id"] = request_id;
    pending_turn_inputs_.push_back(std::move(steer));
    LOG_INFO("[turn/interject] question " + request_id +
             " resolved by user interjection on turn " + active_turn_id_);
    return {
        TurnSteerStatus::Accepted,
        active_turn_id_,
        "accepted; question resolved by interjection",
    };
}

TurnSteerResult AgentLoop::interrupt_turn(
    const std::string& expected_turn_id,
    const UserInput& input) {
    if (expected_turn_id.empty()) {
        return {
            TurnSteerStatus::InvalidInput,
            {},
            "expected turn id is required",
        };
    }
    if (!has_meaningful_user_input(input)) {
        return {
            TurnSteerStatus::InvalidInput,
            {},
            "steering input is empty",
        };
    }

    std::string interrupted_turn_id;
    std::size_t promised_inputs = 0;
    {
        // Lock order is intentionally active_turn_mu_ -> queue_mu_. No worker
        // path holds queue_mu_ while acquiring active_turn_mu_.
        std::lock_guard<std::mutex> turn_lk(active_turn_mu_);
        if (!active_turn_accepting_ || active_turn_id_.empty()) {
            return {
                busy_.load()
                    ? TurnSteerStatus::NonSteerable
                    : TurnSteerStatus::NoActiveTurn,
                {},
                busy_.load()
                    ? "the busy operation is not steerable"
                    : "no active turn",
            };
        }
        if (expected_turn_id != active_turn_id_) {
            return {
                TurnSteerStatus::TurnMismatch,
                active_turn_id_,
                "expected turn does not match the active turn",
            };
        }

        interrupted_turn_id = active_turn_id_;
        const std::size_t input_count = pending_turn_inputs_.size() + 1;
        std::lock_guard<std::mutex> queue_lk(queue_mu_);
        if (priority_task_queue_.size() + input_count >
            kMaxPendingTurnSteers) {
            return {
                TurnSteerStatus::QueueFull,
                interrupted_turn_id,
                "interrupting turn queue is full",
            };
        }

        auto promise_follow_up = [&](UserInput follow_up) {
            if (!follow_up.metadata.is_object()) {
                follow_up.metadata = nlohmann::json::object();
            }
            follow_up.metadata["turn_interrupt"] = true;
            follow_up.metadata["interrupted_turn_id"] = interrupted_turn_id;

            WorkerTask task;
            task.kind = WorkerTask::Kind::Chat;
            task.input = std::move(follow_up);
            priority_task_queue_.push(std::move(task));
            ++promised_inputs;
        };

        while (!pending_turn_inputs_.empty()) {
            promise_follow_up(std::move(pending_turn_inputs_.front()));
            pending_turn_inputs_.pop_front();
        }
        promise_follow_up(input);

        // Close acceptance before setting abort, so a concurrent soft steer
        // cannot be acknowledged and then discarded during turn teardown.
        active_turn_accepting_ = false;
        turn_interrupt_requested_.store(true);
        abort_signal_.request();
    }

    wake_active_provider_retry();
    queue_cv_.notify_one();
    LOG_INFO("[turn/interrupt] accepted " +
             std::to_string(promised_inputs) +
             " high-priority input(s) for active turn " +
             interrupted_turn_id);
    return {
        TurnSteerStatus::Accepted,
        interrupted_turn_id,
        "accepted; interrupt requested",
    };
}

std::string AgentLoop::active_turn_id() const {
    std::lock_guard<std::mutex> lk(active_turn_mu_);
    return active_turn_accepting_ ? active_turn_id_ : std::string{};
}

void AgentLoop::begin_active_turn(const std::string& turn_id) {
    std::lock_guard<std::mutex> lk(active_turn_mu_);
    pending_turn_inputs_.clear();
    active_turn_id_ = turn_id;
    active_turn_accepting_ = !turn_id.empty();
}

void AgentLoop::append_interrupted_turn_context(const std::string& turn_id) {
    ChatMessage marker;
    marker.role = "user";
    marker.content =
        "<turn_aborted>\n"
        "The user interrupted the previous turn on purpose to submit new "
        "instructions. Any running tools or commands may have partially "
        "executed; inspect their state before retrying.\n"
        "</turn_aborted>";
    marker.metadata = nlohmann::json{
        {"hidden_goal_context", true},
        {"turn_interrupt_marker", true},
        {"interrupted_turn_id", turn_id},
    };
    ensure_user_message_identity(marker);
    messages_.push_back(marker);
    if (session_manager_) session_manager_->on_message(marker);
    LOG_INFO("[turn/interrupt] recorded interrupted-turn context for " + turn_id);
}

void AgentLoop::commit_turn_steering_input(
    UserInput input,
    const std::string& turn_id) {
    ChatMessage message;
    message.role = "user";
    message.content = std::move(input.text);
    message.content_parts = std::move(input.content_parts);
    message.metadata = std::move(input.metadata);
    if (!message.metadata.is_object()) {
        message.metadata = nlohmann::json::object();
    }
    if (!input.display_text.empty() && input.display_text != message.content) {
        message.metadata["display_text"] = std::move(input.display_text);
    }
    message.metadata["turn_steer"] = true;
    message.metadata["turn_id"] = turn_id;
    ensure_user_message_identity(message);

    messages_.push_back(message);
    if (session_manager_) {
        session_manager_->on_message(message);
    }
    emit_session_summary_updated();

    const std::string display = message.metadata.value(
        "display_text", message.content);
    if (callbacks_.on_message) {
        callbacks_.on_message("user", display, false);
    }

    nlohmann::json event = {
        {"role", "user"},
        {"content", message.content},
        {"is_tool", false},
        {"id", message.uuid},
        {"metadata", message.metadata},
    };
    if (message.content_parts.is_array() && !message.content_parts.empty()) {
        event["content_parts"] = message.content_parts;
    }
    events_.emit(SessionEventKind::Message, std::move(event));
    LOG_INFO("[turn/steer] committed input to active turn " + turn_id);
}

bool AgentLoop::drain_active_turn_inputs(bool close_if_empty) {
    std::deque<UserInput> pending;
    std::string turn_id;
    {
        std::lock_guard<std::mutex> lk(active_turn_mu_);
        if (!active_turn_accepting_ || active_turn_id_.empty()) return false;
        if (pending_turn_inputs_.empty()) {
            if (close_if_empty) {
                active_turn_accepting_ = false;
                active_turn_id_.clear();
            }
            return false;
        }
        turn_id = active_turn_id_;
        pending.swap(pending_turn_inputs_);
    }

    for (auto& input : pending) {
        commit_turn_steering_input(std::move(input), turn_id);
    }
    return true;
}

std::size_t AgentLoop::close_active_turn_and_discard() {
    std::lock_guard<std::mutex> lk(active_turn_mu_);
    const std::size_t dropped = pending_turn_inputs_.size();
    pending_turn_inputs_.clear();
    active_turn_accepting_ = false;
    active_turn_id_.clear();
    return dropped;
}

} // namespace acecode
