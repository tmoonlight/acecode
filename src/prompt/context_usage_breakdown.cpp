#include "context_usage_breakdown.hpp"

#include "system_prompt.hpp"
#include "../commands/compact.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <numeric>
#include <nlohmann/json.hpp>

namespace acecode {

namespace {

using Field = int ContextUsageBreakdown::*;

constexpr std::array<Field, 7> kCategoryFields = {
    &ContextUsageBreakdown::system_prompt,
    &ContextUsageBreakdown::project_rules,
    &ContextUsageBreakdown::skills,
    &ContextUsageBreakdown::builtin_tools,
    &ContextUsageBreakdown::mcp_tools,
    &ContextUsageBreakdown::conversation,
    &ContextUsageBreakdown::dynamic_context,
};

int clamped_token_estimate(std::size_t bytes) {
    const std::size_t tokens = (bytes + 3) / 4;
    return tokens > static_cast<std::size_t>(std::numeric_limits<int>::max())
        ? std::numeric_limits<int>::max()
        : static_cast<int>(tokens);
}

int tool_schema_tokens(const std::vector<ToolDef>& tools) {
    if (tools.empty()) return 0;
    return clamped_token_estimate(
        serialize_tool_schemas_for_prompt_cache(tools).size());
}

int read_non_negative_int(const nlohmann::json& value,
                          const char* key) {
    auto it = value.find(key);
    if (it == value.end() || !it->is_number_integer()) return 0;
    const std::int64_t raw = it->get<std::int64_t>();
    if (raw <= 0) return 0;
    return raw > std::numeric_limits<int>::max()
        ? std::numeric_limits<int>::max()
        : static_cast<int>(raw);
}

} // namespace

ContextUsageBreakdown estimate_context_usage_breakdown(
    const std::string& system_prompt,
    const std::vector<ChatMessage>& conversation_messages,
    const std::vector<ChatMessage>& mutable_context_messages,
    std::size_t project_rules_bytes,
    std::size_t skills_bytes,
    const std::vector<ToolDef>& builtin_tools,
    const std::vector<ToolDef>& mcp_tools) {
    ContextUsageBreakdown result;

    if (!system_prompt.empty()) {
        ChatMessage system;
        system.role = "system";
        system.content = system_prompt;
        result.system_prompt = estimate_message_tokens({system});
    }

    result.conversation = estimate_message_tokens(conversation_messages);
    result.builtin_tools = tool_schema_tokens(builtin_tools);
    result.mcp_tools = tool_schema_tokens(mcp_tools);

    const int mutable_total = estimate_message_tokens(mutable_context_messages);
    int remaining = mutable_total;
    result.project_rules = (std::min)(
        remaining, clamped_token_estimate(project_rules_bytes));
    remaining -= result.project_rules;
    result.skills = (std::min)(
        remaining, clamped_token_estimate(skills_bytes));
    remaining -= result.skills;
    result.dynamic_context = remaining;

    result.has_data = context_usage_breakdown_total(result) > 0;
    return result;
}

ContextUsageBreakdown reconcile_context_usage_breakdown(
    const ContextUsageBreakdown& raw,
    int provider_prompt_tokens) {
    ContextUsageBreakdown result;
    if (provider_prompt_tokens <= 0) return result;

    std::array<std::int64_t, kCategoryFields.size()> weights{};
    std::int64_t weight_total = 0;
    for (std::size_t i = 0; i < kCategoryFields.size(); ++i) {
        weights[i] = (std::max)(0, raw.*kCategoryFields[i]);
        weight_total += weights[i];
    }

    if (weight_total <= 0) {
        result.conversation = provider_prompt_tokens;
        result.has_data = true;
        return result;
    }

    struct Remainder {
        std::size_t index = 0;
        std::int64_t value = 0;
    };
    std::array<Remainder, kCategoryFields.size()> remainders{};
    int assigned = 0;

    for (std::size_t i = 0; i < kCategoryFields.size(); ++i) {
        const std::int64_t numerator =
            weights[i] * static_cast<std::int64_t>(provider_prompt_tokens);
        const int scaled = static_cast<int>(numerator / weight_total);
        result.*kCategoryFields[i] = scaled;
        assigned += scaled;
        remainders[i] = Remainder{i, numerator % weight_total};
    }

    std::stable_sort(
        remainders.begin(), remainders.end(),
        [](const Remainder& a, const Remainder& b) {
            return a.value > b.value;
        });

    int residual = provider_prompt_tokens - assigned;
    for (int i = 0; i < residual; ++i) {
        const std::size_t index =
            remainders[static_cast<std::size_t>(i) % remainders.size()].index;
        ++(result.*kCategoryFields[index]);
    }

    result.has_data = true;
    return result;
}

int context_usage_breakdown_total(
    const ContextUsageBreakdown& breakdown) {
    std::int64_t total = 0;
    for (Field field : kCategoryFields) {
        total += (std::max)(0, breakdown.*field);
    }
    return total > std::numeric_limits<int>::max()
        ? std::numeric_limits<int>::max()
        : static_cast<int>(total);
}

nlohmann::json context_usage_breakdown_to_json(
    const ContextUsageBreakdown& breakdown) {
    return nlohmann::json{
        {"system_prompt", breakdown.system_prompt},
        {"project_rules", breakdown.project_rules},
        {"skills", breakdown.skills},
        {"builtin_tools", breakdown.builtin_tools},
        {"mcp_tools", breakdown.mcp_tools},
        {"conversation", breakdown.conversation},
        {"dynamic_context", breakdown.dynamic_context},
        {"has_data", breakdown.has_data},
    };
}

ContextUsageBreakdown context_usage_breakdown_from_json(
    const nlohmann::json& value) {
    ContextUsageBreakdown result;
    if (!value.is_object()) return result;

    result.system_prompt = read_non_negative_int(value, "system_prompt");
    result.project_rules = read_non_negative_int(value, "project_rules");
    result.skills = read_non_negative_int(value, "skills");
    result.builtin_tools = read_non_negative_int(value, "builtin_tools");
    result.mcp_tools = read_non_negative_int(value, "mcp_tools");
    result.conversation = read_non_negative_int(value, "conversation");
    result.dynamic_context = read_non_negative_int(value, "dynamic_context");
    result.has_data = value.value("has_data", false);
    return result;
}

} // namespace acecode
