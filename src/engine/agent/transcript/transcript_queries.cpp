#include "transcript_queries.hpp"
#include "agent/compaction/compact.hpp"
#include "session/compact_checkpoint.hpp"
#include "session/session_rewind.hpp"
#include "session/tool_metadata_codec.hpp"
#include "session/turn_timing.hpp"
#include "session/turn_net_diff.hpp"
#include "agent/event_payload/message_payload.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <utility>

namespace acecode::agent::detail {

bool is_transcript_bookkeeping(const ChatMessage& message) {
    return message.is_meta || is_file_checkpoint_message(message) ||
           is_compact_checkpoint_message(message) ||
           is_turn_timing_message(message) || is_turn_net_diff_message(message) ||
           web::is_hidden_goal_context_message(message);
}

const ChatMessage* trailing_transcript_message(
    const std::vector<ChatMessage>& messages, bool user_only) {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        // Match the transcript's invisible bookkeeping records. Never skip a
        // visible non-user unless an explicit user-abort marker allows retry.
        if (is_transcript_bookkeeping(*it)) continue;
        if (user_only && it->role != "user") continue;
        return &*it;
    }
    return nullptr;
}

nlohmann::json build_transcript_replace_payload(
    const std::vector<ChatMessage>& messages,
    const CompactResult& result) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& msg : messages) {
        if (is_file_checkpoint_message(msg)) continue;
        if (is_compact_checkpoint_message(msg)) continue;
        if (is_content_replacement_message(msg)) continue;
        if (is_turn_timing_message(msg)) continue;
        if (web::is_hidden_goal_context_message(msg)) continue;
        arr.push_back(web::chat_message_to_payload_json(msg));
    }
    return nlohmann::json{
        {"messages", std::move(arr)},
        {"messages_compressed", result.messages_compressed},
        {"estimated_tokens_saved", result.estimated_tokens_saved},
    };
}

bool should_persist_trajectory_event(const SessionEvent& event) {
    switch (event.kind) {
    case SessionEventKind::Token:
    case SessionEventKind::Reasoning:
    case SessionEventKind::ToolUpdate:
    case SessionEventKind::ToolEnd:
    case SessionEventKind::TurnDiff:
    case SessionEventKind::TranscriptReplace:
    case SessionEventKind::GoalUpdated:
    case SessionEventKind::GoalCleared:
    case SessionEventKind::TodoUpdated:
    case SessionEventKind::SessionUpdated:
    case SessionEventKind::Done:
    case SessionEventKind::BusyChanged:
        return false;
    case SessionEventKind::AgentProgress: {
        const std::string phase = event.payload.value("phase", std::string{});
        return phase == "model_retry" || phase == "compacting";
    }
    case SessionEventKind::Message: {
        const std::string role = event.payload.value("role", std::string{});
        return role != "tool_call" && role != "tool_result";
    }
    default:
        return true;
    }
}

} // namespace acecode::agent::detail
