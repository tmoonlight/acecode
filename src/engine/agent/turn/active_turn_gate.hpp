#pragma once

#include "llm/llm_provider.hpp"
#include "session/session_client.hpp"

#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace acecode {
class AbortSignal;
class AskUserQuestionPrompter;
}

namespace acecode::agent {

class AgentTaskQueue;

// Cross-thread input acceptance. Lock order is gate -> queue -> AbortSignal;
// no queue-held operation may reenter the gate. Question responses also enter
// their prompter only under this gate, before publishing the accepted input.
class ActiveTurnGate {
public:
    static constexpr std::size_t kMaxPendingInputs = 128;
    struct DrainedInputs {
        std::string turn_id;
        std::deque<UserInput> inputs;
    };
    ActiveTurnGate(const std::atomic<bool>& busy, AbortSignal& abort,
                   std::atomic<bool>& interrupt_requested)
        : busy_(busy), abort_(abort), interrupt_requested_(interrupt_requested) {}
    TurnSteerResult steer(const std::string& expected, const UserInput& input);
    TurnSteerResult interject(const std::string& request_id, const UserInput& input,
                             const std::string& expected, AskUserQuestionPrompter* prompter);
    TurnSteerResult interrupt(const std::string& expected, const UserInput& input,
                              AgentTaskQueue& queue, std::size_t& promised_inputs);
    // 「立即发送」:client_message_id 对应的插话还在 pending_ 里时,把 pending_ 里排在
    // 它前面的、它自己以及后面的插话都提成高优先级回合并打断当前回合(不追加新输入,
    // 不会重复);已被模型读到 / 不存在 → NotPending。
    TurnSteerResult promote(const std::string& expected, const std::string& client_message_id,
                            AgentTaskQueue& queue, std::size_t& promised_inputs);
    // 撤回 pending_ 里 client_message_id 对应的插话;已被读到 / 不存在 → NotPending。
    TurnSteerResult withdraw(const std::string& client_message_id);
    std::string id() const;
    bool has_pending() const;
    void begin(const std::string& id);
    DrainedInputs drain(bool close_if_empty);
    std::size_t close_and_discard();

private:
    // 以下两个都要求调用方持有 mu_。
    std::optional<TurnSteerResult> reject_inactive_locked(const std::string& expected) const;
    TurnSteerResult promise_pending_and_abort_locked(const UserInput* extra, AgentTaskQueue& queue,
                                                     std::size_t& promised_inputs);
    const std::atomic<bool>& busy_;
    AbortSignal& abort_;
    std::atomic<bool>& interrupt_requested_;
    mutable std::mutex mu_;
    std::string id_;
    bool accepting_ = false;
    std::deque<UserInput> pending_;
};

} // namespace acecode::agent
