#include "agent/agent_loop.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/mailbox/agent_mailbox.hpp"
#include "agent/turn/active_turn_gate.hpp"
#include "agent/worker/agent_task_queue.hpp"
#include "session/session_manager.hpp"
#include "tool/background_tasks.hpp"
#include "utils/logger.hpp"
#include <utility>

namespace acecode {

TurnSteerResult AgentLoop::steer_input(
    const std::string& expected_turn_id, const UserInput& input) {
    auto result = active_turn_gate_->steer(expected_turn_id, input);
    // Codex InputQueueActivity::Steer: steered user input ends agent_wait early.
    if (result.accepted()) {
        mailbox_->notify_steer();
        // 插话只在模型边界注入:正在跑的前台命令 / 等待中的子代理让路转到后台
        // (不中断),消息才能马上送到模型。
        request_background(BackgroundOrigin::UserMessage);
    }
    return result;
}

TurnSteerResult AgentLoop::interject_question(
    const std::string& request_id, const UserInput& input, const std::string& expected_turn_id) {
    auto result = active_turn_gate_->interject(
        request_id, input, expected_turn_id, ask_prompter_.get());
    if (result.accepted()) mailbox_->notify_steer();
    return result;
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

TurnSteerResult AgentLoop::send_steer_now(
    const std::string& expected_turn_id, const std::string& client_message_id) {
    std::size_t promised_inputs = 0;
    auto result = active_turn_gate_->promote(
        expected_turn_id, client_message_id, *task_queue_, promised_inputs);
    if (result.accepted()) {
        wake_active_provider_retry();
        task_queue_->notify();
        LOG_INFO("[turn/steer] send-now promoted " + std::to_string(promised_inputs) +
                 " pending input(s) and interrupted turn " + result.turn_id);
    }
    return result;
}

TurnSteerResult AgentLoop::withdraw_steer(const std::string& client_message_id) {
    return active_turn_gate_->withdraw(client_message_id);
}

std::string AgentLoop::active_turn_id() const {
    return active_turn_gate_->id();
}

} // namespace acecode
