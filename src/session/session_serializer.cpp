#include "session_serializer.hpp"
#include <nlohmann/json.hpp>

namespace acecode {

std::string serialize_message(const ChatMessage& msg) {
    nlohmann::json j;
    j["role"] = msg.role;

    if (!msg.content.empty()) {
        j["content"] = msg.content;
    }

    if (!msg.tool_calls.is_null() && !msg.tool_calls.empty()) {
        j["tool_calls"] = msg.tool_calls;
    }

    if (!msg.tool_call_id.empty()) {
        j["tool_call_id"] = msg.tool_call_id;
    }

    // dump(-1) produces compact single-line JSON with no extra whitespace
    return j.dump(-1);
}

ChatMessage deserialize_message(const std::string& line) {
    auto j = nlohmann::json::parse(line);
    ChatMessage msg;

    if (j.contains("role") && j["role"].is_string()) {
        msg.role = j["role"].get<std::string>();
    }

    if (j.contains("content") && j["content"].is_string()) {
        msg.content = j["content"].get<std::string>();
    }

    if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
        msg.tool_calls = j["tool_calls"];
    }

    if (j.contains("tool_call_id") && j["tool_call_id"].is_string()) {
        msg.tool_call_id = j["tool_call_id"].get<std::string>();
    }

    return msg;
}

} // namespace acecode
