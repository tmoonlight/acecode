#include "inter_agent_message.hpp"

namespace acecode::mesh {

const char* inter_agent_message_type_name(InterAgentMessageType type) {
    switch (type) {
    case InterAgentMessageType::NewTask: return "NEW_TASK";
    case InterAgentMessageType::FinalAnswer: return "FINAL_ANSWER";
    case InterAgentMessageType::Message: break;
    }
    return "MESSAGE";
}

std::optional<InterAgentMessageType> parse_inter_agent_message_type(const std::string& name) {
    if (name == "MESSAGE") return InterAgentMessageType::Message;
    if (name == "NEW_TASK") return InterAgentMessageType::NewTask;
    if (name == "FINAL_ANSWER") return InterAgentMessageType::FinalAnswer;
    return std::nullopt;
}

std::string render_inter_agent_body(const InterAgentEnvelope& envelope) {
    return std::string("Message Type: ") + inter_agent_message_type_name(envelope.type) +
           "\nTask name: " + envelope.recipient +
           "\nSender: " + envelope.sender +
           "\nPayload:\n" + envelope.payload;
}

std::string render_inter_agent_message(const InterAgentEnvelope& envelope) {
    return "<inter_agent_message>\n" + render_inter_agent_body(envelope) +
           "\n</inter_agent_message>";
}

nlohmann::json inter_agent_metadata(const InterAgentEnvelope& envelope) {
    nlohmann::json value = {
        {"type", inter_agent_message_type_name(envelope.type)},
        {"sender", envelope.sender},
        {"recipient", envelope.recipient},
    };
    if (!envelope.sender_session_id.empty()) {
        value["sender_session_id"] = envelope.sender_session_id;
    }
    if (!envelope.final_status.empty()) value["status"] = envelope.final_status;
    return value;
}

std::optional<InterAgentEnvelope> inter_agent_envelope_from_metadata(
    const nlohmann::json& metadata) {
    if (!metadata.is_object()) return std::nullopt;
    const auto it = metadata.find(kInterAgentMetadataKey);
    if (it == metadata.end() || !it->is_object()) return std::nullopt;
    const auto type = parse_inter_agent_message_type(it->value("type", std::string{}));
    if (!type) return std::nullopt;
    InterAgentEnvelope envelope;
    envelope.type = *type;
    envelope.sender = it->value("sender", std::string{});
    envelope.recipient = it->value("recipient", std::string{});
    envelope.sender_session_id = it->value("sender_session_id", std::string{});
    envelope.final_status = it->value("status", std::string{});
    return envelope;
}

bool is_inter_agent_message(const ChatMessage& message) {
    return message.role == "user" &&
           inter_agent_envelope_from_metadata(message.metadata).has_value();
}

} // namespace acecode::mesh
