#include "compact.hpp"
#include "../utils/logger.hpp"

#include <sstream>
#include <algorithm>

namespace acecode {

int estimate_message_tokens(const std::vector<ChatMessage>& messages) {
    int total_chars = 0;
    for (const auto& msg : messages) {
        total_chars += static_cast<int>(msg.content.size());
        // Tool calls JSON also counts
        if (!msg.tool_calls.is_null() && !msg.tool_calls.empty()) {
            total_chars += static_cast<int>(msg.tool_calls.dump().size());
        }
    }
    return total_chars / 4; // rough estimate: ~4 chars per token
}

bool should_auto_compact(const std::vector<ChatMessage>& messages, int context_window) {
    int estimated = estimate_message_tokens(messages);
    return estimated > static_cast<int>(context_window * 0.8);
}

CompactResult compact_context(
    LlmProvider& provider,
    AgentLoop& agent_loop,
    TuiState& state,
    int keep_turns
) {
    CompactResult result;
    auto& messages = agent_loop.messages_mut();

    // Count user/assistant turn pairs from the end to find the keep boundary
    int turns_found = 0;
    int keep_from = static_cast<int>(messages.size()); // index to keep from

    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
        if (messages[i].role == "user") {
            turns_found++;
            if (turns_found >= keep_turns) {
                keep_from = i;
                break;
            }
        }
    }

    // Need at least some messages to compress
    if (keep_from <= 0) {
        result.error = "Not enough conversation history to compact.";
        return result;
    }

    // Collect messages to summarize (indices 0..keep_from-1)
    std::vector<ChatMessage> to_summarize(messages.begin(), messages.begin() + keep_from);

    int tokens_before = estimate_message_tokens(to_summarize);
    if (tokens_before < 200) {
        result.error = "Not enough conversation history to compact.";
        return result;
    }

    // Build the summarization prompt
    std::ostringstream summary_prompt;
    summary_prompt << "Summarize the following conversation concisely. "
                   << "Preserve all technical details: file paths, variable names, "
                   << "function names, decisions made, code snippets discussed, "
                   << "and any errors encountered. "
                   << "Keep the summary structured and actionable.\n\n";

    for (const auto& msg : to_summarize) {
        if (msg.role == "system") continue; // skip system prompts
        summary_prompt << "[" << msg.role << "]: " << msg.content << "\n";
    }

    // Call the LLM to generate summary
    std::vector<ChatMessage> summary_messages;
    ChatMessage sys_msg;
    sys_msg.role = "system";
    sys_msg.content = "You are a conversation summarizer. Be concise but preserve all technical details.";
    summary_messages.push_back(sys_msg);

    ChatMessage user_msg;
    user_msg.role = "user";
    user_msg.content = summary_prompt.str();
    summary_messages.push_back(user_msg);

    ChatResponse summary_resp;
    try {
        summary_resp = provider.chat(summary_messages, {}); // no tools for summarization
    } catch (const std::exception& e) {
        LOG_ERROR("Compact: summarization failed: " + std::string(e.what()));
        result.error = "Summarization failed: " + std::string(e.what());
        return result;
    }

    if (summary_resp.content.empty()) {
        result.error = "Summarization returned empty response.";
        return result;
    }

    // Build the summary message
    ChatMessage summary_msg;
    summary_msg.role = "user";
    summary_msg.content = "[Conversation summary]\n" + summary_resp.content;

    // Replace messages: remove 0..keep_from-1, insert summary at front
    std::vector<ChatMessage> new_messages;
    new_messages.push_back(summary_msg);
    new_messages.insert(new_messages.end(), messages.begin() + keep_from, messages.end());

    int tokens_after = estimate_message_tokens(new_messages);

    result.performed = true;
    result.messages_compressed = keep_from;
    result.estimated_tokens_saved = std::max(0, tokens_before - estimate_message_tokens({summary_msg}));

    // Update agent loop messages
    messages = std::move(new_messages);

    // Update TUI conversation display
    {
        std::lock_guard<std::mutex> lk(state.mu);
        // Keep last N entries proportional to keep_turns
        int tui_keep = 0;
        int tui_turns = 0;
        for (int i = static_cast<int>(state.conversation.size()) - 1; i >= 0; --i) {
            if (state.conversation[i].role == "user") {
                tui_turns++;
                if (tui_turns >= keep_turns) {
                    tui_keep = i;
                    break;
                }
            }
        }
        std::vector<TuiState::Message> new_conv;
        new_conv.push_back({"system", "[Conversation summary]\n" + summary_resp.content, false});
        new_conv.insert(new_conv.end(),
                        state.conversation.begin() + tui_keep,
                        state.conversation.end());
        state.conversation = std::move(new_conv);
        state.chat_follow_tail = true;
    }

    return result;
}

} // namespace acecode
