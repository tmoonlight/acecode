#include "agent/agent_loop.hpp"
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
    return active_turn_gate_->interject(request_id, input, expected_turn_id, ask_prompter_);
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

void AgentLoop::begin_active_turn(const std::string& turn_id) {
    active_turn_gate_->begin(turn_id);
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
    auto drained = active_turn_gate_->drain(close_if_empty);
    for (auto& input : drained.inputs) {
        commit_turn_steering_input(std::move(input), drained.turn_id);
    }
    return !drained.inputs.empty();
}

std::size_t AgentLoop::close_active_turn_and_discard() {
    return active_turn_gate_->close_and_discard();
}

} // namespace acecode
