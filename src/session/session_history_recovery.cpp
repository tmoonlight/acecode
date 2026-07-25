#include "session_history_recovery.hpp"

#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace acecode {

namespace {

constexpr const char* kInterruptedToolResult =
    "[Interrupted] ACECode recovered this session after the tool call ended "
    "without a persisted result. The execution outcome is unknown. Inspect "
    "the current state before deciding whether to retry; do not assume success "
    "or failure.";

bool has_assistant_content(const ChatMessage& msg) {
    return !msg.content.empty() ||
           !msg.reasoning_content.empty() ||
           (msg.content_parts.is_array() && !msg.content_parts.empty());
}

std::optional<std::string> valid_tool_call_id(const nlohmann::json& call) {
    if (!call.is_object() ||
        !call.contains("id") ||
        !call["id"].is_string()) {
        return std::nullopt;
    }
    const std::string id = call["id"].get<std::string>();
    if (id.empty() ||
        !call.contains("function") ||
        !call["function"].is_object()) {
        return std::nullopt;
    }
    const auto& function = call["function"];
    if (!function.contains("name") ||
        !function["name"].is_string() ||
        function["name"].get<std::string>().empty()) {
        return std::nullopt;
    }
    return id;
}

ChatMessage interrupted_tool_result(const std::string& tool_call_id) {
    ChatMessage result;
    result.role = "tool";
    result.tool_call_id = tool_call_id;
    result.content = kInterruptedToolResult;
    result.metadata = nlohmann::json{
        {"session_recovery", {
            {"version", 1},
            {"kind", "missing_tool_result"},
            {"outcome", "unknown"},
        }},
    };
    return result;
}

} // namespace

ProviderHistoryRecoveryResult recover_provider_history(
    const std::vector<ChatMessage>& messages) {
    ProviderHistoryRecoveryResult result;
    result.messages.reserve(messages.size());

    std::vector<std::string> pending_order;
    std::unordered_set<std::string> pending_ids;
    std::unordered_set<std::string> seen_call_ids;
    std::unordered_set<std::string> seen_result_ids;

    const auto flush_missing_results = [&]() {
        for (const auto& id : pending_order) {
            if (!pending_ids.count(id)) continue;
            result.messages.push_back(interrupted_tool_result(id));
            seen_result_ids.insert(id);
            ++result.stats.synthesized_tool_results;
        }
        pending_order.clear();
        pending_ids.clear();
    };

    for (const auto& original : messages) {
        // System rows are movable provider context, not a turn boundary.
        // Keeping the pending set open lets a real tool result that follows an
        // injected system row win over a synthetic interruption placeholder.
        if (original.role != "tool" && original.role != "system") {
            flush_missing_results();
        }

        if (original.role == "assistant") {
            ChatMessage assistant = original;
            nlohmann::json calls = nlohmann::json::array();
            if (original.tool_calls.is_array()) {
                calls = original.tool_calls;
            } else if (original.tool_calls.is_object()) {
                calls.push_back(original.tool_calls);
            }

            nlohmann::json retained = nlohmann::json::array();
            for (const auto& call : calls) {
                const auto id = valid_tool_call_id(call);
                if (!id.has_value()) {
                    ++result.stats.malformed_tool_calls;
                    continue;
                }
                if (!seen_call_ids.insert(*id).second) {
                    ++result.stats.duplicate_tool_calls;
                    continue;
                }
                retained.push_back(call);
                pending_order.push_back(*id);
                pending_ids.insert(*id);
            }

            if (retained.empty()) {
                assistant.tool_calls = nlohmann::json();
                if (!has_assistant_content(assistant)) {
                    ++result.stats.empty_assistant_messages;
                    continue;
                }
            } else {
                assistant.tool_calls = std::move(retained);
            }
            result.messages.push_back(std::move(assistant));
            continue;
        }

        if (original.role == "tool") {
            const std::string& id = original.tool_call_id;
            if (!id.empty() && pending_ids.erase(id) > 0) {
                result.messages.push_back(original);
                seen_result_ids.insert(id);
                continue;
            }
            if (!id.empty() && seen_result_ids.count(id)) {
                ++result.stats.duplicate_tool_results;
            } else if (pending_ids.empty()) {
                ++result.stats.standalone_tool_results;
            } else {
                ++result.stats.unexpected_tool_results;
            }
            continue;
        }

        result.messages.push_back(original);
    }

    flush_missing_results();
    return result;
}

} // namespace acecode
