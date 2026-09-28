#pragma once

#include "session_client.hpp"

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace acecode {

enum class SessionAttentionState {
    Read,
    Unread,
    InProgress,
};

struct SessionAttentionRecord {
    bool busy = false;
    std::uint64_t update_cursor = 0;
    std::uint64_t read_cursor = 0;
    std::int64_t updated_at_ms = 0;
};

const char* to_string(SessionAttentionState state);
std::optional<SessionAttentionState> parse_session_attention_state(const std::string& value);

SessionAttentionState session_attention_state_for(const SessionAttentionRecord& record);

bool session_event_has_user_visible_output(SessionEventKind kind,
                                           const nlohmann::json& payload);

SessionAttentionRecord apply_session_attention_event(
    SessionAttentionRecord record,
    SessionEventKind kind,
    const nlohmann::json& payload,
    std::uint64_t cursor,
    std::int64_t timestamp_ms);

SessionAttentionRecord mark_session_attention_read(
    SessionAttentionRecord record,
    std::uint64_t cursor,
    std::int64_t timestamp_ms);

// 用户在会话右键菜单里「标记为未读」:把已读游标退到最新输出之前,
// 之后任何一次标记已读(cursor=0 或最新游标)都能恢复。从没有过输出的会话
// 也要能标成未读,所以 update_cursor 为 0 时补成 1。
SessionAttentionRecord mark_session_attention_unread(
    SessionAttentionRecord record,
    std::int64_t timestamp_ms);

} // namespace acecode
