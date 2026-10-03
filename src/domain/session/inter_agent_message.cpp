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

std::string inter_agent_payload_from_content(const std::string& content) {
    static const std::string kPayload = "\nPayload:\n";
    static const std::string kClose = "\n</inter_agent_message>";
    const auto start = content.find(kPayload);
    if (start == std::string::npos) return {};
    std::string payload = content.substr(start + kPayload.size());
    if (payload.size() >= kClose.size() &&
        payload.compare(payload.size() - kClose.size(), kClose.size(), kClose) == 0) {
        payload.resize(payload.size() - kClose.size());
    }
    return payload;
}

std::string inter_agent_display_text(const ChatMessage& message) {
    if (message.role != "user") return {};
    const auto envelope = inter_agent_envelope_from_metadata(message.metadata);
    if (!envelope) return {};
    std::string header;
    switch (envelope->type) {
    case InterAgentMessageType::NewTask:
        header = "Task from " + envelope->sender + " to " + envelope->recipient;
        break;
    case InterAgentMessageType::Message:
        header = "Message from " + envelope->sender + " to " + envelope->recipient;
        break;
    case InterAgentMessageType::FinalAnswer:
        header = "Agent " + envelope->sender + " " +
                 (envelope->final_status.empty() ? std::string("finished")
                                                 : envelope->final_status);
        break;
    }
    const std::string payload = inter_agent_payload_from_content(message.content);
    return payload.empty() ? header : header + ":\n" + payload;
}

} // namespace acecode::mesh
