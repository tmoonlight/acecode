#include "context_usage.hpp"

#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>

namespace acecode {

namespace {

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
