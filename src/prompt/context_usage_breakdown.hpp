#pragma once

#include "../provider/llm_provider.hpp"

#include <cstddef>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace acecode {

// Estimate the seven category weights from the same request components that
// are sent to the provider. project_rules_bytes and skills_bytes are exact
// content partitions within mutable_context_messages; remaining message bytes
// and envelopes belong to dynamic_context.
ContextUsageBreakdown estimate_context_usage_breakdown(
    const std::string& system_prompt,
    const std::vector<ChatMessage>& conversation_messages,
    const std::vector<ChatMessage>& mutable_context_messages,
    std::size_t project_rules_bytes,
    std::size_t skills_bytes,
    const std::vector<ToolDef>& builtin_tools,
    const std::vector<ToolDef>& mcp_tools);

// Scale raw category weights to provider_prompt_tokens using deterministic
// largest-remainder allocation. Positive authoritative usage always yields a
// breakdown whose fields sum exactly to provider_prompt_tokens.
ContextUsageBreakdown reconcile_context_usage_breakdown(
    const ContextUsageBreakdown& raw,
    int provider_prompt_tokens);

int context_usage_breakdown_total(const ContextUsageBreakdown& breakdown);

nlohmann::json context_usage_breakdown_to_json(
    const ContextUsageBreakdown& breakdown);
ContextUsageBreakdown context_usage_breakdown_from_json(
    const nlohmann::json& value);

} // namespace acecode
