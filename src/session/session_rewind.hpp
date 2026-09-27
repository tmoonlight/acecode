#pragma once

#include "llm/llm_provider.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

struct RewindTarget {
    size_t message_index = 0;
    std::string message_uuid;
    std::string preview;
    bool has_stable_uuid = false;
};

// Assign identity fields needed by /rewind. No-op for non-user messages and
// for messages that already carry ids from a resumed session.
void ensure_user_message_identity(ChatMessage& msg);

// True only for user-authored turns that make sense as rewind boundaries.
bool is_rewind_selectable_user_message(const ChatMessage& msg);

// File checkpoint meta messages are persisted in JSONL but should not render as
// normal chat rows and should not be considered conversation content.
bool is_file_checkpoint_message(const ChatMessage& msg);

std::vector<RewindTarget> collect_rewind_targets(const std::vector<ChatMessage>& messages);

std::vector<ChatMessage> retained_prefix_before_index(
    const std::vector<ChatMessage>& messages,
    size_t target_index);

// Index of the last message that a fork should keep when the user forks at
// `target_index`, or nullopt when nothing before the target should be kept.
//
// Scanning skips records that are not part of the conversation (meta messages,
// file checkpoints, turn timing, turn net diff) because keeping one as the last
// line of a forked session would expose diagnostics as if they were chat.
//
// It also refuses to stop on an assistant message that declares tool calls:
// the matching tool results always follow the call, so stopping there would
// leave a dangling call that providers reject. Stopping on a tool result is
// safe because the pair stays intact.
//
// `target_index` must be a valid index into `messages`.
std::optional<size_t> resolve_fork_anchor_index(
    const std::vector<ChatMessage>& messages,
    size_t target_index);

// Plain text of a user prompt that a fork response hands back for composer
// refill. Prefers `content`; when it is empty the text may live only in
// structured `content_parts` (multimodal input), so text parts are joined
// with newlines. Non-text parts (images, files) are not refilled.
std::string fork_restored_prompt_text(const ChatMessage& msg);

std::string rewind_prefill_text(const ChatMessage& msg);

std::string rewind_preview_text(const ChatMessage& msg, size_t max_bytes = 80);

} // namespace acecode
