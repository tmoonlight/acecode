#pragma once

// Inter-agent envelopes for the mesh swarm mode. The body is Codex's
// InterAgentMessage / InterAgentCompletionMessage text; ACECode deliberately
// wraps it in <inter_agent_message> and persists it as a user-role message
// (Codex uses the assistant role, which Anthropic/DeepSeek-style providers
// reject when two assistant messages become adjacent).

#include "llm/llm_provider.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace acecode::mesh {

enum class InterAgentMessageType { Message, NewTask, FinalAnswer };

const char* inter_agent_message_type_name(InterAgentMessageType type);
std::optional<InterAgentMessageType> parse_inter_agent_message_type(const std::string& name);

// Metadata key holding {type, sender, recipient, sender_session_id}.
inline constexpr const char* kInterAgentMetadataKey = "inter_agent";

struct InterAgentEnvelope {
    InterAgentMessageType type = InterAgentMessageType::Message;
    std::string recipient;          // canonical path ("Task name")
    std::string sender;             // canonical path
    std::string sender_session_id;  // UI routing only, never model-facing
    std::string payload;
    // FINAL_ANSWER only: completed | errored | interrupted.
    std::string final_status;
};

// "Message Type: ...\nTask name: ...\nSender: ...\nPayload:\n..." (Codex text).
std::string render_inter_agent_body(const InterAgentEnvelope& envelope);
// The body wrapped in <inter_agent_message> tags; this is the model content.
std::string render_inter_agent_message(const InterAgentEnvelope& envelope);
nlohmann::json inter_agent_metadata(const InterAgentEnvelope& envelope);

// Reads metadata.inter_agent; nullopt for ordinary messages.
std::optional<InterAgentEnvelope> inter_agent_envelope_from_metadata(
    const nlohmann::json& metadata);
bool is_inter_agent_message(const ChatMessage& message);

} // namespace acecode::mesh
