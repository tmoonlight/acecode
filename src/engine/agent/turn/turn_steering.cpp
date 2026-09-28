#include "agent/agent_loop.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/turn/active_turn_gate.hpp"
#include "agent/worker/agent_task_queue.hpp"
#include "session/session_manager.hpp"
#include "utils/logger.hpp"
#include <utility>

namespace acecode {

TurnSteerResult AgentLoop::steer_input(
    const std::string& expected_turn_id, const UserInput& input) {
    return active_turn_gate_->steer(expected_turn_id, input);
}

TurnSteerResult AgentLoop::interject_question(
    const std::string& request_id, const UserInput& input, const std::string& expected_turn_id) {
    return active_turn_gate_->interject(request_id, input, expected_turn_id, ask_prompter_.get());
}

TurnSteerResult AgentLoop::interrupt_turn(
    const std::string& expected_turn_id, const UserInput& input) {
    std::size_t promised_inputs = 0;
    auto result = active_turn_gate_->interrupt(
        expected_turn_id, input, *task_queue_, promised_inputs);
    if (result.accepted()) {
        wake_active_provider_retry();
        task_queue_->notify();
        LOG_INFO("[turn/interrupt] accepted " + std::to_string(promised_inputs) +
                 " high-priority input(s) for active turn " + result.turn_id);
    }
    return result;
}

std::string AgentLoop::active_turn_id() const {
    return active_turn_gate_->id();
}

} // namespace acecode
