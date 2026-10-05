#pragma once

#include "llm/llm_provider.hpp"
#include <string>
#include <vector>

namespace acecode {

struct SessionRecentActivity {
    std::string last_user_message_at;
    std::string last_turn_outcome;
};

// Visible human input only; internal mesh messages and compact summaries do not
// make a conversation recent. Reuses the existing persisted message flags.
bool is_recent_user_message(const ChatMessage& message);
SessionRecentActivity recent_activity_from_messages(const std::vector<ChatMessage>& messages);
// Reads backwards using the existing bounded JSONL scanner. Does not rewrite
// old metadata or load an entire long transcript into memory.
SessionRecentActivity read_session_recent_activity(const std::string& jsonl_path);

} // namespace acecode
