#pragma once

#include "../provider/llm_provider.hpp"

#include <cstddef>
#include <vector>

namespace acecode {

// Aggregate-only diagnostics. These counters are safe to log because they do
// not contain transcript text, tool arguments, paths, or provider secrets.
struct ProviderHistoryRecoveryStats {
    std::size_t malformed_tool_calls = 0;
    std::size_t duplicate_tool_calls = 0;
    std::size_t synthesized_tool_results = 0;
    std::size_t standalone_tool_results = 0;
    std::size_t unexpected_tool_results = 0;
    std::size_t duplicate_tool_results = 0;
    std::size_t empty_assistant_messages = 0;

    bool changed() const {
        return malformed_tool_calls > 0 ||
               duplicate_tool_calls > 0 ||
               synthesized_tool_results > 0 ||
               standalone_tool_results > 0 ||
               unexpected_tool_results > 0 ||
               duplicate_tool_results > 0 ||
               empty_assistant_messages > 0;
    }
};

struct ProviderHistoryRecoveryResult {
    std::vector<ChatMessage> messages;
    ProviderHistoryRecoveryStats stats;
};

// Repair the logical provider projection without rewriting the append-only
// human transcript. Missing tool results become explicit outcome-unknown
// placeholders; invalid result rows are omitted from this projection only.
ProviderHistoryRecoveryResult recover_provider_history(
    const std::vector<ChatMessage>& messages);

} // namespace acecode
