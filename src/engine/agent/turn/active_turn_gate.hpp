#pragma once

#include "llm/llm_provider.hpp"
#include "session/session_client.hpp"

#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
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
    std::string id() const;
    bool has_pending() const;
    void begin(const std::string& id);
    DrainedInputs drain(bool close_if_empty);
    std::size_t close_and_discard();

private:
    const std::atomic<bool>& busy_;
    AbortSignal& abort_;
    std::atomic<bool>& interrupt_requested_;
    mutable std::mutex mu_;
    std::string id_;
    bool accepting_ = false;
    std::deque<UserInput> pending_;
};

} // namespace acecode::agent
