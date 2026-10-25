#pragma once

#include "llm/llm_provider.hpp"
#include <cstdint>
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
// Process-local revision for new visible conversation content. SessionManager
// records successful main-session appends only, never history reads/rewrites.
// No callbacks or borrowed session state are retained.
std::uint64_t session_conversation_activity_revision();
void note_session_conversation_message(const ChatMessage& message);
SessionRecentActivity recent_activity_from_messages(const std::vector<ChatMessage>& messages);
// Reads backwards using the existing bounded JSONL scanner. Does not rewrite
// old metadata or load an entire long transcript into memory.
SessionRecentActivity read_session_recent_activity(const std::string& jsonl_path);
// First 240 bytes of the latest assistant reply in the current turn, read backwards
// and stopping at the latest visible user message. Feeds the companion pet's
// "done" card when the daemon no longer holds the live reply (after a restart).
std::string read_session_last_reply(const std::string& jsonl_path);

} // namespace acecode
