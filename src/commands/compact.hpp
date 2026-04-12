#pragma once

#include "../provider/llm_provider.hpp"
#include "../agent_loop.hpp"
#include "../tui_state.hpp"
#include "../utils/token_tracker.hpp"

#include <string>

namespace acecode {

struct CompactResult {
    bool performed = false;
    int messages_compressed = 0;
    int estimated_tokens_saved = 0;
    std::string error;
};

// Estimate total tokens from a message list (characters / 4 heuristic)
int estimate_message_tokens(const std::vector<ChatMessage>& messages);

// Perform context compaction:
// - Summarizes early messages using the LLM
// - Preserves the most recent `keep_turns` user/assistant turns
// - Replaces compressed messages with a summary system message
// - Updates both AgentLoop messages and TuiState conversation
CompactResult compact_context(
    LlmProvider& provider,
    AgentLoop& agent_loop,
    TuiState& state,
    int keep_turns = 4
);

// Check if auto-compact should trigger (estimated tokens > 80% of context_window)
bool should_auto_compact(const std::vector<ChatMessage>& messages, int context_window);

} // namespace acecode
