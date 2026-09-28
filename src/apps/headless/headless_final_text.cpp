#include "headless_final_text.hpp"

#include <nlohmann/json.hpp>

namespace acecode::headless {

namespace {

bool is_rejected_text_tool_call(const ChatMessage& message) {
    return message.metadata.is_object() &&
           message.metadata.contains("text_tool_call_rejected");
}

bool is_blank(const std::string& text) {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

} // namespace

std::string headless_final_assistant_text(const std::vector<ChatMessage>& messages,
                                          std::size_t baseline) {
    bool saw_later_assistant = false;
    for (std::size_t i = messages.size(); i > baseline; --i) {
        const auto& message = messages[i - 1];
        if (message.role != "assistant") continue;
        if (is_rejected_text_tool_call(message)) {
            // 回合以被拒告终(它之后没有任何 assistant):没有可用的最终回复。
            if (!saw_later_assistant) return {};
            continue;
        }
        saw_later_assistant = true;
        if (!is_blank(message.content)) return message.content;
    }
    return {};
}

} // namespace acecode::headless
