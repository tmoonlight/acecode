#pragma once

#include "llm/llm_provider.hpp"
#include "session/event_dispatcher.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace acecode { class SessionManager; struct CompactResult; }

namespace acecode::agent::detail {

bool is_transcript_bookkeeping(const ChatMessage& message);

const ChatMessage* trailing_transcript_message(
    const std::vector<ChatMessage>& messages, bool user_only = false);

nlohmann::json build_transcript_replace_payload(
    const std::vector<ChatMessage>& messages,
    const CompactResult& result);

bool should_persist_trajectory_event(const SessionEvent& event);

} // namespace acecode::agent::detail
