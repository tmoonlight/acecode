#include "turn_usage_accountant.hpp"
#include "agent/callbacks_slot.hpp"
#include "agent/detail/agent_payloads.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "session/event_dispatcher.hpp"
#include "session/session_manager.hpp"
#include "session/token_tracker.hpp"
#include "tool/tool_executor.hpp"

namespace acecode::agent {
using detail::accumulate_turn_usage;
using detail::model_step_usage_to_json;

void TurnUsageAccountant::accept(TurnUsageRecord& record, const TokenUsage& usage,
                                 SessionManager* session) {
    const auto callbacks = callbacks_.snapshot();
    accumulate_turn_usage(record.aggregate, record.initialized, usage);
    context_tokens_.store(usage.total_tokens > 0 ? usage.total_tokens : usage.prompt_tokens,
                          std::memory_order_relaxed);
    goal_.account_usage(session, usage.total_tokens, false);
    if (callbacks.on_usage) callbacks.on_usage(usage);
    if (session) session->record_token_usage(usage);
    events_.emit(SessionEventKind::Usage, model_step_usage_to_json(usage));
}

TokenUsage TurnUsageAccountant::estimate(TurnUsageRecord& record,
    const ChatResponse& response, const ApiRequestBundle& bundle, SessionManager* session) {
    const auto callbacks = callbacks_.snapshot();
    TokenUsage estimated_usage;
    estimated_usage.prompt_tokens = estimate_message_tokens(bundle.messages_with_system);
    ChatMessage estimated_response;
    if (response.has_tool_calls()) {
        estimated_response = ToolExecutor::format_assistant_tool_calls(response);
    } else {
        estimated_response.role = "assistant";
        estimated_response.content = response.content;
        if (response.content_parts.is_array() && !response.content_parts.empty()) {
            estimated_response.content_parts = response.content_parts;
        }
        estimated_response.reasoning_content = response.reasoning_content;
    }
    estimated_usage.completion_tokens = estimate_message_tokens({estimated_response});
    estimated_usage.total_tokens = estimated_usage.prompt_tokens + estimated_usage.completion_tokens;
    estimated_usage.has_data = false;
    estimated_usage.context_breakdown = reconcile_context_usage_breakdown(
        bundle.context_usage_estimate,
        estimated_usage.prompt_tokens);
    accumulate_turn_usage(
        record.aggregate, record.initialized, estimated_usage);
    goal_.account_usage(session, estimated_usage.total_tokens, false);
    if (callbacks.on_usage) callbacks.on_usage(estimated_usage);
    if (session) session->record_token_usage(estimated_usage);
    return estimated_usage;
}
} // namespace acecode::agent
