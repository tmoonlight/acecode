#pragma once

#include "llm/llm_provider.hpp"

#include <cstddef>
#include <vector>

namespace acecode {

// Aggregate-only diagnostics. These counters are safe to log because they do
// not contain transcript text, tool arguments, paths, or provider secrets.
struct ProviderHistoryRecoveryStats {
    std::size_t malformed_tool_calls = 0;
    // Includes same-message duplicates dropped and cross-message IDs remapped.
    std::size_t duplicate_tool_calls = 0;
    std::size_t synthesized_tool_results = 0;
    std::size_t standalone_tool_results = 0;
    std::size_t unexpected_tool_results = 0;
    std::size_t duplicate_tool_results = 0;
    std::size_t empty_assistant_messages = 0;
    // Results of dropped malformed calls, replaced by a "not executed" note.
    std::size_t malformed_call_results = 0;

    bool changed() const {
        return malformed_tool_calls > 0 ||
               duplicate_tool_calls > 0 ||
               synthesized_tool_results > 0 ||
               standalone_tool_results > 0 ||
               unexpected_tool_results > 0 ||
               duplicate_tool_results > 0 ||
               empty_assistant_messages > 0 ||
               malformed_call_results > 0;
    }
};

struct ProviderHistoryRecoveryResult {
    std::vector<ChatMessage> messages;
    ProviderHistoryRecoveryStats stats;
};

// 调用被当成损坏丢掉、但它的报错结果还在历史里时,投影在这一组工具结果之后
// 放一条 user 消息说明「调用没有执行」。只由内容决定、逐字节稳定。
inline constexpr const char* kMalformedToolCallNote =
    "[Tool call not executed] A tool call in the previous assistant message had an "
    "empty or malformed tool name (not a plain name from the tool list), so nothing "
    "was executed. Call tools through the tool-calling interface, using an exact tool "
    "name from the tool list and JSON arguments.";

// Repair the logical provider projection without rewriting the append-only
// human transcript. Missing tool results become explicit outcome-unknown
// placeholders; invalid result rows are omitted from this projection only.
// Calls with an empty or corrupted name (llm/tool_call_corruption) are
// dropped; if such a call's error result was persisted, it becomes one
// kMalformedToolCallNote user message after the group's results, so the
// request does not end on an orphaned assistant message and the model still
// sees that its call failed — without the corrupted name being echoed back.
ProviderHistoryRecoveryResult recover_provider_history(
    const std::vector<ChatMessage>& messages);

} // namespace acecode
