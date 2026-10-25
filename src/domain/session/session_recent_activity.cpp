#include "session_recent_activity.hpp"
#include "inter_agent_message.hpp"
#include "jsonl_scanner.hpp"
#include "session_serializer.hpp"
#include "session_user_message_search.hpp"
#include "turn_timing.hpp"
#include "utils/encoding.hpp"
#include <atomic>

namespace acecode {
namespace {
std::atomic<std::uint64_t> conversation_activity_revision{0};

bool consume(const ChatMessage& message, SessionRecentActivity& result) {
    if (result.last_turn_outcome.empty() && is_turn_timing_message(message)) {
        result.last_turn_outcome = decode_turn_timing(message.metadata.at("turn_timing"))->status;
    }
    if (!is_recent_user_message(message)) return false;
    result.last_user_message_at = message.timestamp;
    return true;
}
} // namespace

bool is_recent_user_message(const ChatMessage& message) {
    const auto tool = message.metadata.is_object() ? message.metadata.find("source_tool") : message.metadata.end();
    const bool tool_injected = tool != message.metadata.end() && tool->is_string() && !tool->get_ref<const std::string&>().empty();
    return !tool_injected && is_searchable_visible_user_message(message) && !mesh::is_inter_agent_message(message);
}

std::uint64_t session_conversation_activity_revision() {
    return conversation_activity_revision.load();
}

void note_session_conversation_message(const ChatMessage& message) {
    if (message.is_meta || message.is_compact_summary || !message.subtype.empty()) return;
    const bool reply = message.role == "assistant" &&
        (!message.content.empty() || (message.content_parts.is_array() && !message.content_parts.empty()));
    if (is_recent_user_message(message) || reply) ++conversation_activity_revision;
}

SessionRecentActivity recent_activity_from_messages(const std::vector<ChatMessage>& messages) {
    SessionRecentActivity result;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (consume(*it, result)) break;
    }
    return result;
}

SessionRecentActivity read_session_recent_activity(const std::string& jsonl_path) {
    SessionRecentActivity result;
    SessionFileReader reader(jsonl_path);
    if (!reader.valid()) return result;
    JsonlScanner scanner(reader, true);
    while (const auto record = scanner.next()) {
        try {
            if (consume(deserialize_message(record->text), result)) break;
        } catch (const nlohmann::json::exception&) {
            // A damaged line is not a message or a completion signal.
        }
    }
    return result;
}

std::string read_session_last_reply(const std::string& jsonl_path) {
    SessionFileReader reader(jsonl_path);
    if (!reader.valid()) return {};
    JsonlScanner scanner(reader, true);
    while (const auto record = scanner.next()) {
        try {
            const auto message = deserialize_message(record->text);
            if (is_recent_user_message(message)) break;
            if (message.role == "assistant" && !message.content.empty())
                return truncate_utf8_prefix(message.content, 240);
        } catch (const nlohmann::json::exception&) {
            // A damaged line is skipped like in read_session_recent_activity.
        }
    }
    return {};
}
} // namespace acecode
