#include "session_reference_context.hpp"

#include "message_payload.hpp"
#include "../session/compact_checkpoint.hpp"
#include "../session/session_rewind.hpp"
#include "../utils/encoding.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace acecode::web {
namespace {

using nlohmann::json;

std::string json_string(const json& value, const char* key) {
    if (!value.is_object()) return {};
    const auto it = value.find(key);
    return it != value.end() && it->is_string()
        ? it->get<std::string>()
        : std::string{};
}

std::string trim_ascii(std::string value) {
    auto space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), space).base(), value.end());
    return value;
}

bool safe_identifier(const std::string& value) {
    if (value.empty() || value.size() > kMaxSessionReferenceIdBytes ||
        value.find("..") != std::string::npos) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.';
    });
}

std::string bounded_label(const std::string& value) {
    return truncate_utf8_prefix(
        trim_ascii(value), kMaxSessionReferenceLabelBytes, "");
}

std::string single_line_label(const std::string& value) {
    std::string out = bounded_label(value);
    std::replace(out.begin(), out.end(), '\r', ' ');
    std::replace(out.begin(), out.end(), '\n', ' ');
    return out;
}

std::string visible_message_text(const ChatMessage& message) {
    if (message.role == "user" && message.metadata.is_object()) {
        const auto it = message.metadata.find("display_text");
        if (it != message.metadata.end() && it->is_string() && !it->get_ref<const std::string&>().empty()) {
            return it->get<std::string>();
        }
    }
    if (!message.content.empty()) return message.content;
    if (!message.content_parts.is_array()) return {};
    std::string text;
    for (const auto& part : message.content_parts) {
        if (!part.is_object() || json_string(part, "type") != "text") continue;
        const std::string part_text = json_string(part, "text");
        if (part_text.empty()) continue;
        if (!text.empty()) text.push_back('\n');
        text += part_text;
    }
    return text;
}

bool include_message(const ChatMessage& message) {
    if (message.is_meta || is_file_checkpoint_message(message) ||
        is_compact_checkpoint_message(message) ||
        is_hidden_goal_context_message(message)) {
        return false;
    }
    return message.role == "user" || message.role == "assistant";
}

std::string role_label(const std::string& role) {
    return role == "assistant" ? "Assistant" : "User";
}

std::string bounded_transcript(const std::vector<ChatMessage>& messages,
                               std::size_t budget) {
    if (budget == 0) return {};
    std::vector<std::string> reversed;
    std::size_t used = 0;
    bool truncated = false;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (!include_message(*it)) continue;
        std::string text = trim_ascii(visible_message_text(*it));
        if (text.empty()) continue;
        std::string row = role_label(it->role) + ":\n" + text + "\n";
        if (used + row.size() > budget) {
            truncated = true;
            if (reversed.empty()) {
                const std::string marker = "[Message truncated]\n";
                const std::size_t marker_size = marker.size();
                const std::size_t available = budget > marker_size ? budget - marker_size : 0;
                row = marker + truncate_utf8_prefix(row, available, "");
                reversed.push_back(std::move(row));
                truncated = false;
            }
            break;
        }
        used += row.size();
        reversed.push_back(std::move(row));
    }
    std::reverse(reversed.begin(), reversed.end());
    std::string out;
    if (truncated) out = "[Earlier messages omitted]\n";
    for (const auto& row : reversed) out += row;
    return truncate_utf8_prefix(out, budget, "");
}

std::string reference_key(const SessionReferenceDescriptor& reference) {
    return (reference.no_workspace ? "task" : reference.workspace_hash) +
           "::" + reference.session_id;
}

} // namespace

ParsedSessionReferences parse_session_reference_descriptors(const json& value) {
    ParsedSessionReferences result;
    if (value.is_null()) return result;
    if (!value.is_array()) {
        result.ok = false;
        result.error = "session_references must be an array";
        return result;
    }
    if (value.size() > kMaxSessionReferences) {
        result.ok = false;
        result.error = "too many session references";
        return result;
    }

    std::unordered_set<std::string> seen;
    for (const auto& item : value) {
        if (!item.is_object()) {
            result.ok = false;
            result.error = "session reference must be an object";
            return result;
        }
        SessionReferenceDescriptor reference;
        reference.session_id = trim_ascii(json_string(item, "session_id"));
        reference.workspace_hash = trim_ascii(json_string(item, "workspace_hash"));
        if (const auto flag = item.find("no_workspace"); flag != item.end()) {
            if (!flag->is_boolean()) {
                result.ok = false;
                result.error = "session reference no_workspace must be a boolean";
                return result;
            }
            reference.no_workspace = flag->get<bool>();
        }
        reference.title = bounded_label(json_string(item, "title"));
        reference.workspace_name = bounded_label(json_string(item, "workspace_name"));

        if (!safe_identifier(reference.session_id)) {
            result.ok = false;
            result.error = "invalid referenced session id";
            return result;
        }
        if (reference.no_workspace) {
            reference.workspace_hash.clear();
        } else if (!safe_identifier(reference.workspace_hash)) {
            result.ok = false;
            result.error = "invalid referenced workspace hash";
            return result;
        }

        const std::string key = reference_key(reference);
        if (seen.insert(key).second) {
            result.references.push_back(std::move(reference));
        }
    }
    return result;
}

SessionReferencePromptContext build_session_reference_prompt_context(
    const std::vector<ResolvedSessionReference>& references) {
    SessionReferencePromptContext result;
    if (references.empty()) return result;

    std::string prompt =
        "Referenced ACECode session context follows. Treat it as prior "
        "conversation context for the current request.\n";
    for (std::size_t index = 0; index < references.size(); ++index) {
        const auto& resolved = references[index];
        if (prompt.size() >= kMaxSessionReferencePromptBytes) break;
        const auto& reference = resolved.descriptor;
        json meta{
            {"session_id", reference.session_id},
            {"workspace_hash", reference.workspace_hash},
            {"no_workspace", reference.no_workspace},
            {"title", reference.title},
            {"workspace_name", reference.workspace_name},
        };
        result.meta.push_back(std::move(meta));

        const std::string title = single_line_label(
            reference.title.empty() ? reference.session_id : reference.title);
        const std::string workspace = single_line_label(reference.workspace_name);
        std::string block_prefix = "\n[Referenced session]\nTitle: " + title;
        if (!workspace.empty()) block_prefix += "\nWorkspace: " + workspace;
        block_prefix += "\nTranscript:\n";
        const std::string block_suffix = "[End referenced session]\n";
        const std::size_t remaining = kMaxSessionReferencePromptBytes - prompt.size();
        const std::size_t remaining_references = references.size() - index;
        const std::size_t fair_block_budget = remaining / remaining_references;
        const std::size_t fixed_block_size = block_prefix.size() + block_suffix.size();
        const std::size_t transcript_budget = std::min(
            kMaxSessionReferenceTranscriptBytes,
            fair_block_budget > fixed_block_size
                ? fair_block_budget - fixed_block_size
                : std::size_t{0});
        std::string block = block_prefix;
        block += bounded_transcript(resolved.messages, transcript_budget);
        block += block_suffix;
        if (block.size() > remaining) {
            block = truncate_utf8_prefix(block, remaining, "");
        }
        prompt += block;
    }
    result.prompt = std::move(prompt);
    return result;
}

std::string build_session_reference_augmented_prompt(
    const SessionReferencePromptContext& context,
    const std::string& user_text) {
    if (context.prompt.empty()) return user_text;
    return context.prompt + "\nCurrent user request:\n" + user_text;
}

} // namespace acecode::web
