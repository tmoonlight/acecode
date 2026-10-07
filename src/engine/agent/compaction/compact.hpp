#pragma once

// token 估算、消息谓词与上下文阈值已下沉到 llm/(P2-02);这里继续 include 它们,
// 让 agent_loop / main / 测试等既有使用方的写法不变。
#include "llm/context_thresholds.hpp"
#include "llm/llm_provider.hpp"
#include "llm/message_predicates.hpp"
#include "llm/token_estimate.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace acecode {

constexpr std::size_t COMPACT_USER_MESSAGE_MAX_TOKENS = 20000;

struct CompactResult {
    bool performed = false;
    int messages_compressed = 0;
    int estimated_tokens_saved = 0;
    int compaction_request_items_removed = 0;
    int compaction_request_retries = 0;
    std::string summary_text;
    std::vector<ChatMessage> compacted_messages;
    std::string error;
};

std::vector<ChatMessage> build_compacted_history(
    const std::vector<ChatMessage>& messages,
    const std::string& summary_text,
    std::size_t max_user_message_tokens = COMPACT_USER_MESSAGE_MAX_TOKENS);

std::vector<ChatMessage> normalize_messages_for_api(
    const std::vector<ChatMessage>& messages);

bool is_context_overflow_error(const ProviderErrorInfo& info);
bool is_context_overflow_error(const std::string& error_message);
bool is_retryable_compaction_error(const ProviderErrorInfo& info);

// Validate a summarization reply before it is installed as the compact
// summary. Returns "" when acceptable, otherwise a short reason:
// "tool_calls" (native tool calls), "empty" (blank after trimming) or
// "tool_call_markup" (tool-call tags anywhere outside code fences/inline
// code, or provider diagnostics for filtered textual calls). There is
// intentionally no minimum length: a valid Chinese summary
// can be only a few characters.
std::string compact_summary_rejection_reason(const ChatResponse& response);

// Rejected summaries are retried at most this many times (3 attempts total)
// before compaction fails without installing anything.
constexpr int kMaxInvalidCompactSummaryRetries = 2;

using CompactRetryCallback =
    std::function<void(const ProviderErrorInfo& info, bool waiting)>;

// Already-built model-facing input. Keep its ordering, tool aliases, reasoning
// and structured parts unchanged; only the summary instruction is appended.
// Tool-free fallback retains these same bytes; initial system/snapshot messages
// stay fixed while overflow recovery may prune the remaining history.
struct CompactRequestPrefix {
    std::vector<ChatMessage> messages;
    std::vector<ToolDef> tools;
    ChatRequestOptions request_options;
};

// Insert rebuilt request-local context at Codex's handoff boundary: before the
// last real user message, or before the compact summary when no real user
// message remains. Existing history content is never rewritten.
void insert_context_before_last_real_user_or_summary(
    std::vector<ChatMessage>& messages,
    std::vector<ChatMessage> context);

// Run Codex-compatible local compaction. initial_context contains stable
// base/session instructions that are always retained during overflow retries.
// A supported request_prefix is used verbatim, then retried without tools on
// invalid summaries or incompatibility before applying normal overflow pruning.
CompactResult compact_messages(
    LlmProvider& provider,
    const std::vector<ChatMessage>& messages,
    const std::vector<ChatMessage>& initial_context = {},
    bool is_auto = false,
    std::atomic<bool>* abort_flag = nullptr,
    CompactRetryCallback on_retry = {},
    const CompactRequestPrefix* request_prefix = nullptr);

} // namespace acecode
