#include "micro_compact.hpp"
#include "compact.hpp"
#include "../utils/logger.hpp"
#include "../utils/uuid.hpp"

#include <algorithm>

namespace acecode {

const std::set<std::string> COMPACTABLE_TOOLS = {
    "file_read", "file_write", "file_edit",
    "bash", "shell",
    "grep", "glob",
    "web_fetch"
};

namespace {

// Find the tool name for a given tool_call_id by scanning assistant messages
std::string find_tool_name_for_call_id(
    const std::vector<ChatMessage>& messages,
    int start, int end,
    const std::string& tool_call_id
) {
    for (int i = start; i < end; ++i) {
        const auto& msg = messages[i];
        if (msg.role == "assistant" && !msg.tool_calls.is_null() && msg.tool_calls.is_array()) {
            for (const auto& tc : msg.tool_calls) {
                if (tc.contains("id") && tc["id"].is_string() &&
                    tc["id"].get<std::string>() == tool_call_id) {
                    if (tc.contains("function") && tc["function"].contains("name")) {
                        return tc["function"]["name"].get<std::string>();
                    }
                }
            }
        }
    }
    return "";
}

} // namespace

MicroCompactResult run_micro_compact(
    std::vector<ChatMessage>& messages,
    int boundary_start,
    int keep_assistant_turns
) {
    MicroCompactResult result;
    int n = static_cast<int>(messages.size());

    // Find the cutoff: messages before the last `keep_assistant_turns` assistant messages
    int assistant_count = 0;
    int cutoff_index = n; // messages at or after this index are "recent"

    for (int i = n - 1; i >= boundary_start; --i) {
        if (messages[i].role == "assistant") {
            assistant_count++;
            if (assistant_count >= keep_assistant_turns) {
                cutoff_index = i;
                break;
            }
        }
    }

    // Micro-compact only clears tool results before the cutoff
    static const std::string placeholder = "[Old tool result content cleared]";

    for (int i = boundary_start; i < cutoff_index; ++i) {
        auto& msg = messages[i];
        if (msg.role != "tool") continue;
        if (msg.content == placeholder) continue; // already cleared
        if (msg.content.empty()) continue;

        // Check if the tool is in the compactable set
        std::string tool_name = find_tool_name_for_call_id(messages, boundary_start, cutoff_index, msg.tool_call_id);
        if (tool_name.empty() || COMPACTABLE_TOOLS.find(tool_name) == COMPACTABLE_TOOLS.end()) {
            continue;
        }

        int original_chars = static_cast<int>(msg.content.size());
        int savings = (original_chars - static_cast<int>(placeholder.size())) / 4;
        if (savings <= 0) continue;

        result.estimated_tokens_saved += savings;
        result.tool_results_cleared++;
        result.cleared_tool_call_ids.push_back(msg.tool_call_id);

        msg.content = placeholder;
    }

    result.performed = result.tool_results_cleared > 0;

    if (result.performed) {
        LOG_INFO("Micro-compact: cleared " + std::to_string(result.tool_results_cleared) +
                 " tool results, estimated ~" + std::to_string(result.estimated_tokens_saved) + " tokens saved");
    }

    return result;
}

ChatMessage create_microcompact_boundary_message(
    int pre_tokens,
    int tokens_saved,
    const std::vector<std::string>& cleared_ids
) {
    ChatMessage msg;
    msg.role = "system";
    msg.content = "[Micro-compact boundary]";
    msg.uuid = generate_uuid();
    msg.subtype = "microcompact_boundary";
    msg.timestamp = iso_timestamp();
    msg.is_meta = true;

    nlohmann::json ids_array = nlohmann::json::array();
    for (const auto& id : cleared_ids) {
        ids_array.push_back(id);
    }

    msg.metadata = {
        {"trigger", "auto"},
        {"pre_tokens", pre_tokens},
        {"tokens_saved", tokens_saved},
        {"cleared_tool_call_ids", ids_array}
    };
    return msg;
}

} // namespace acecode
