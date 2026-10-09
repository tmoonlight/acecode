#include "message_payload.hpp"

#include "session/session_serializer.hpp"
#include "utils/sha1.hpp"

namespace acecode::web {

std::string compute_message_id(const ChatMessage& m) {
    if ((m.role == "user" || m.role == "error") && !m.uuid.empty()) {
        return m.uuid;
    }
    // Other roles and legacy records without UUIDs retain content-based IDs.
    std::string buf;
    buf.reserve(m.role.size() + m.content.size() + m.timestamp.size() + 2);
    buf.append(m.role);
    buf.push_back(' ');
    buf.append(m.content);
    buf.push_back(' ');
    buf.append(m.timestamp);
    return sha1_hex(buf);
}

bool is_hidden_goal_context_message(const ChatMessage& m) {
    return m.metadata.is_object() &&
           m.metadata.value("hidden_goal_context", false);
}

nlohmann::json chat_message_to_payload_json(const ChatMessage& m) {
    // 复用 session_serializer 的字段集合 + 顶层 id。
    auto j = serialize_message_json(m);
    j["id"] = compute_message_id(m);
    return j;
}

} // namespace acecode::web
