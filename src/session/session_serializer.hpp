#pragma once

#include "../provider/llm_provider.hpp"
#include <string>

namespace acecode {

// Serialize a ChatMessage to a single-line JSON string (for JSONL storage).
// Empty fields are omitted to save space.
std::string serialize_message(const ChatMessage& msg);

// Deserialize a single-line JSON string back to a ChatMessage.
// Handles missing fields gracefully. Throws on invalid JSON.
ChatMessage deserialize_message(const std::string& line);

} // namespace acecode
