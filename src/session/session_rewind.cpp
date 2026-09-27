#include "session_rewind.hpp"

#include "turn_net_diff.hpp"
#include "turn_timing.hpp"
#include "utils/uuid.hpp"

#include <algorithm>

namespace acecode {

namespace {

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool contains_any_synthetic_tag(const std::string& s) {
    static const char* kTags[] = {
        "<bash-input>",
        "<bash-stdout>",
        "<bash-stderr>",
        "<bash-exit-code>",
        "<local-command-stdout>",
        "<local-command-stderr>",
        "<task-notification>",
        "<tick>",
        "<teammate-message",
    };
    for (const char* tag : kTags) {
        if (s.find(tag) != std::string::npos) return true;
    }
    return false;
}

std::string collapse_ws(std::string s) {
    bool in_space = false;
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        const bool space = (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ');
        if (space) {
            if (!in_space) out.push_back(' ');
            in_space = true;
        } else {
            out.push_back(ch);
            in_space = false;
        }
    }
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Records that carry no conversation content. These are the same predicates
// SessionManager::fork_session_to_new_id filters out when writing the forked
// JSONL, so letting one become the last line of a fork would surface raw
// diagnostics as chat.
bool is_non_conversational_record(const ChatMessage& msg) {
    if (msg.is_meta) return true;
    if (is_file_checkpoint_message(msg)) return true;
    if (is_turn_timing_message(msg)) return true;
    if (is_turn_net_diff_message(msg)) return true;
    return false;
}

bool declares_tool_calls(const ChatMessage& msg) {
    return msg.role == "assistant" &&
           msg.tool_calls.is_array() &&
           !msg.tool_calls.empty();
}

} // namespace

void ensure_user_message_identity(ChatMessage& msg) {
    if (msg.role != "user") return;
    if (msg.uuid.empty()) msg.uuid = generate_uuid();
    if (msg.timestamp.empty()) msg.timestamp = iso_timestamp();
}

bool is_file_checkpoint_message(const ChatMessage& msg) {
    return msg.is_meta && msg.subtype == "file_checkpoint";
}

bool is_rewind_selectable_user_message(const ChatMessage& msg) {
    if (msg.role != "user") return false;
    if (msg.is_meta || msg.is_compact_summary) return false;
    if (msg.content.empty()) return false;
    if (starts_with(msg.content, "!")) return false;
    if (contains_any_synthetic_tag(msg.content)) return false;
    return true;
}

std::vector<RewindTarget> collect_rewind_targets(const std::vector<ChatMessage>& messages) {
    std::vector<RewindTarget> out;
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        if (!is_rewind_selectable_user_message(msg)) continue;
        RewindTarget target;
        target.message_index = i;
        target.message_uuid = msg.uuid;
        target.preview = rewind_preview_text(msg);
        target.has_stable_uuid = !msg.uuid.empty();
        out.push_back(std::move(target));
    }
    return out;
}

std::vector<ChatMessage> retained_prefix_before_index(
    const std::vector<ChatMessage>& messages,
    size_t target_index) {
    const size_t end = std::min(target_index, messages.size());
    return std::vector<ChatMessage>(messages.begin(), messages.begin() + static_cast<std::ptrdiff_t>(end));
}

std::optional<size_t> resolve_fork_anchor_index(
    const std::vector<ChatMessage>& messages,
    size_t target_index) {
    if (target_index >= messages.size()) return std::nullopt;

    for (size_t i = target_index; i-- > 0;) {
        const auto& msg = messages[i];
        if (is_non_conversational_record(msg)) continue;
        // A tool result always follows its call, so an assistant message that
        // declares calls has no results inside the retained prefix. Keep
        // scanning so the fork never ends on an unanswered call.
        if (declares_tool_calls(msg)) continue;
        return i;
    }
    return std::nullopt;
}

std::string fork_restored_prompt_text(const ChatMessage& msg) {
    if (!msg.content.empty()) return msg.content;
    // Multimodal input can leave `content` empty and carry the text only in
    // `content_parts`. Join text parts with newlines; skip attachments.
    if (!msg.content_parts.is_array()) return {};
    std::string text;
    for (const auto& part : msg.content_parts) {
        if (!part.is_object()) continue;
        const auto type_it = part.find("type");
        if (type_it == part.end() || !type_it->is_string() ||
            type_it->get_ref<const std::string&>() != "text") {
            continue;
        }
        const auto text_it = part.find("text");
        if (text_it == part.end() || !text_it->is_string()) continue;
        const std::string& part_text = text_it->get_ref<const std::string&>();
        if (part_text.empty()) continue;
        if (!text.empty()) text.push_back('\n');
        text += part_text;
    }
    return text;
}

std::string rewind_prefill_text(const ChatMessage& msg) {
    if (!is_rewind_selectable_user_message(msg)) return {};
    return msg.content;
}

std::string rewind_preview_text(const ChatMessage& msg, size_t max_bytes) {
    std::string s = collapse_ws(msg.content);
    if (s.size() <= max_bytes) return s;
    if (max_bytes <= 3) return s.substr(0, max_bytes);
    return s.substr(0, max_bytes - 3) + "...";
}

} // namespace acecode
