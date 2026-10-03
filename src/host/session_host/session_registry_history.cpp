// SessionRegistry::restore_loop_history: resume 时把持久化消息装回 AgentLoop。
// 单独成文件让 session_registry.cpp 保持在行数基线内。
#include "session_registry.hpp"

#include "session/compact_checkpoint.hpp"
#include "session/session_rewind.hpp"
#include "session/tool_result_storage.hpp"
#include "session/turn_timing.hpp"
#include "tool/file_state_restore.hpp"

#include <optional>
#include <utility>

namespace acecode {

namespace {

bool is_llm_role(const std::string& role) {
    return role == "user" || role == "assistant" ||
           role == "system" || role == "tool";
}

bool is_transcript_only_message(const ChatMessage& msg) {
    return msg.metadata.is_object() &&
           msg.metadata.value("transcript_only", false);
}

std::optional<std::pair<std::size_t, CompactCheckpoint>>
latest_valid_compact_checkpoint(const std::vector<ChatMessage>& messages) {
    for (std::size_t i = messages.size(); i > 0; --i) {
        auto checkpoint = decode_compact_checkpoint(messages[i - 1]);
        if (checkpoint.has_value()) {
            return std::make_pair(i - 1, std::move(*checkpoint));
        }
    }
    return std::nullopt;
}

void append_model_messages_to_loop(AgentLoop& loop,
                                   const std::vector<ChatMessage>& messages) {
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        if (is_file_checkpoint_message(msg)) continue;
        if (is_content_replacement_message(msg)) continue;
        if (is_turn_timing_message(msg)) continue;
        if (is_compact_checkpoint_message(msg)) continue;

        const bool is_shell_user =
            (msg.role == "user" && !msg.content.empty() && msg.content[0] == '!');
        const bool next_is_result =
            (i + 1 < messages.size() && messages[i + 1].role == "tool_result");
        if (is_shell_user && next_is_result) {
            loop.inject_shell_turn(msg.content.substr(1),
                                   messages[i + 1].content,
                                   "",
                                   0);
            ++i;
            continue;
        }

        if (is_llm_role(msg.role) && !is_transcript_only_message(msg)) {
            loop.push_message(msg);
        }
    }
}

} // namespace

void SessionRegistry::restore_loop_history(
    SessionEntry& entry,
    const std::vector<ChatMessage>& messages) const {
    if (!entry.loop) return;
    restore_file_tool_state_from_messages(messages, entry.loop->cwd());
    entry.loop->clear_messages();

    if (auto checkpoint = latest_valid_compact_checkpoint(messages)) {
        append_model_messages_to_loop(*entry.loop, checkpoint->second.replacement_history);
        if (checkpoint->first + 1 < messages.size()) {
            std::vector<ChatMessage> suffix(
                messages.begin() + static_cast<std::ptrdiff_t>(checkpoint->first + 1),
                messages.end());
            append_model_messages_to_loop(*entry.loop, suffix);
        }
    } else {
        append_model_messages_to_loop(*entry.loop, messages);
    }
}

} // namespace acecode
