#include "active_turn_gate.hpp"

#include "agent/turn/user_turn_message.hpp"
#include "agent/worker/agent_task_queue.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "utils/abort_signal.hpp"
#include "utils/logger.hpp"

#include <utility>

namespace acecode::agent {

using detail::has_meaningful_user_input;

TurnSteerResult ActiveTurnGate::steer(
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

    std::lock_guard<std::mutex> lk(mu_);
    if (!accepting_ || id_.empty()) {
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
    if (expected_turn_id != id_) {
        return {
            TurnSteerStatus::TurnMismatch,
            id_,
            "expected turn does not match the active turn",
        };
    }
    if (pending_.size() >= kMaxPendingInputs) {
        return {
            TurnSteerStatus::QueueFull,
            id_,
            "active turn steering queue is full",
        };
    }

    pending_.push_back(input);
    return {
        TurnSteerStatus::Accepted,
        id_,
        "accepted",
    };
}

TurnSteerResult ActiveTurnGate::interject(
    const std::string& request_id,
    const UserInput& input,
    const std::string& expected_turn_id,
    AskUserQuestionPrompter* prompter) {
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
    if (!prompter) {
        // TUI 走 overlay 通道,提问期间 composer 根本不可达,没有这条路径。
        return {
            TurnSteerStatus::NoPendingQuestion,
            {},
            "this session has no asynchronous question channel",
        };
    }

    // 锁序:只持 mu_ 再进 prompter 的锁。prompter 的 prompt()
    // 跑在工具线程上,从不反过来拿 mu_;worker 主线程的
    // drain_active_turn_inputs 要拿 mu_,但它必须等工具批次
    // 收割完 —— 而收割又要等这里的 notify_response 把问题收掉。所以
    // 在释放锁之前把插话压进 pending_,排序就是确定的。
    std::lock_guard<std::mutex> lk(mu_);
    if (!accepting_ || id_.empty()) {
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
    if (!expected_turn_id.empty() && expected_turn_id != id_) {
        return {
            TurnSteerStatus::TurnMismatch,
            id_,
            "expected turn does not match the active turn",
        };
    }
    if (pending_.size() >= kMaxPendingInputs) {
        return {
            TurnSteerStatus::QueueFull,
            id_,
            "active turn steering queue is full",
        };
    }

    // 先收问题再压输入:notify_response 是 first-wins,问题已被别的客户端
    // 回答 / 已超时 / 已关闭时返回 false,此时不能把文本静默变成普通 steer
    // —— 调用方拿到 NoPendingQuestion 后自己决定走排队还是直接发送。
    AskUserQuestionResponse response;
    response.cancelled = true;
    response.interjected = true;
    if (!prompter->notify_response(request_id, response)) {
        return {
            TurnSteerStatus::NoPendingQuestion,
            id_,
            "the question is no longer pending",
        };
    }

    UserInput steer = input;
    if (!steer.metadata.is_object()) {
        steer.metadata = nlohmann::json::object();
    }
    steer.metadata["question_interjection"] = true;
    steer.metadata["question_request_id"] = request_id;
    pending_.push_back(std::move(steer));
    LOG_INFO("[turn/interject] question " + request_id +
             " resolved by user interjection on turn " + id_);
    return {
        TurnSteerStatus::Accepted,
        id_,
        "accepted; question resolved by interjection",
    };
}

TurnSteerResult ActiveTurnGate::interrupt(
    const std::string& expected_turn_id,
    const UserInput& input,
    AgentTaskQueue& queue,
    std::size_t& promised_inputs) {
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
    promised_inputs = 0;
    {
        // Lock order is intentionally gate -> queue. No worker
        // path holds AgentTaskQueue while acquiring mu_.
        std::lock_guard<std::mutex> turn_lk(mu_);
        if (!accepting_ || id_.empty()) {
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
        if (expected_turn_id != id_) {
            return {
                TurnSteerStatus::TurnMismatch,
                id_,
                "expected turn does not match the active turn",
            };
        }

        interrupted_turn_id = id_;
        const std::size_t input_count = pending_.size() + 1;
        return queue.with_locked([&](AgentTaskQueue::Locked& tasks) -> TurnSteerResult {
            if (tasks.priority_size() + input_count >
                kMaxPendingInputs) {
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
                tasks.push(std::move(task), true);
                ++promised_inputs;
            };

            while (!pending_.empty()) {
                promise_follow_up(std::move(pending_.front()));
                pending_.pop_front();
            }
            promise_follow_up(input);

            // Close acceptance before setting abort, so a concurrent soft steer
            // cannot be acknowledged and then discarded during turn teardown.
            accepting_ = false;
            interrupt_requested_.store(true);
            abort_.request();
            return {
                TurnSteerStatus::Accepted,
                interrupted_turn_id,
                "accepted; interrupt requested",
            };
        });
    }
}

std::string ActiveTurnGate::id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return accepting_ ? id_ : std::string{};
}

void ActiveTurnGate::begin(const std::string& turn_id) {
    std::lock_guard<std::mutex> lk(mu_);
    pending_.clear();
    id_ = turn_id;
    accepting_ = !turn_id.empty();
}

ActiveTurnGate::DrainedInputs ActiveTurnGate::drain(bool close_if_empty) {
    std::lock_guard<std::mutex> lock(mu_);
    DrainedInputs drained;
    if (!accepting_ || id_.empty()) return drained;
    if (pending_.empty()) {
        if (close_if_empty) {
            accepting_ = false;
            id_.clear();
        }
        return drained;
    }
    drained.turn_id = id_;
    drained.inputs.swap(pending_);
    return drained;
}

std::size_t ActiveTurnGate::close_and_discard() {
    std::lock_guard<std::mutex> lk(mu_);
    const std::size_t dropped = pending_.size();
    pending_.clear();
    accepting_ = false;
    id_.clear();
    return dropped;
}

} // namespace acecode::agent
