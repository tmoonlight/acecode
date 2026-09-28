#include "agent_payloads.hpp"
#include "session/session_manager.hpp"
#include "utils/encoding.hpp"
#include "prompt/context_usage_breakdown.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <utility>

namespace acecode::agent::detail {

nlohmann::json build_agent_progress_payload(
    const std::string& phase,
    const std::string& label,
    const std::string& detail,
    const std::string& tool,
    const std::string& tool_call_id,
    int tool_index,
    std::int64_t started_at_ms) {
    nlohmann::json payload;
    payload["phase"] = phase;
    payload["label"] = label;
    if (!detail.empty()) payload["detail"] = detail;
    if (!tool.empty()) payload["tool"] = tool;
    if (!tool_call_id.empty()) payload["tool_call_id"] = tool_call_id;
    if (tool_index >= 0) payload["tool_index"] = tool_index;
    if (started_at_ms > 0) payload["started_at_ms"] = started_at_ms;
    return payload;
}

nlohmann::json model_step_usage_to_json(const TokenUsage& usage) {
    nlohmann::json value = {
        {"prompt_tokens", usage.prompt_tokens},
        {"completion_tokens", usage.completion_tokens},
        {"total_tokens", usage.total_tokens},
        {"cache_read_tokens", usage.cache_read_tokens},
        {"cache_write_tokens", usage.cache_write_tokens},
        {"reasoning_tokens", usage.reasoning_tokens},
        {"has_data", usage.has_data},
    };
    if (usage.context_breakdown.has_data) {
        value["context_breakdown"] =
            context_usage_breakdown_to_json(usage.context_breakdown);
    }
    return value;
}

void accumulate_turn_usage(TokenUsage& aggregate,
                           bool& initialized,
                           const TokenUsage& step) {
    aggregate.prompt_tokens += step.prompt_tokens;
    aggregate.completion_tokens += step.completion_tokens;
    aggregate.total_tokens += step.total_tokens;
    aggregate.cache_read_tokens += step.cache_read_tokens;
    aggregate.cache_write_tokens += step.cache_write_tokens;
    aggregate.reasoning_tokens += step.reasoning_tokens;

    auto& total_context = aggregate.context_breakdown;
    const auto& step_context = step.context_breakdown;
    total_context.system_prompt += step_context.system_prompt;
    total_context.project_rules += step_context.project_rules;
    total_context.skills += step_context.skills;
    total_context.builtin_tools += step_context.builtin_tools;
    total_context.mcp_tools += step_context.mcp_tools;
    total_context.conversation += step_context.conversation;
    total_context.dynamic_context += step_context.dynamic_context;
    total_context.has_data = total_context.has_data || step_context.has_data;

    aggregate.has_data = initialized
        ? aggregate.has_data && step.has_data
        : step.has_data;
    initialized = true;
}

const char* text_tool_call_outcome_name(TextToolCallDiagnostic::Outcome outcome) {
    switch (outcome) {
    case TextToolCallDiagnostic::Outcome::Recovered: return "recovered";
    case TextToolCallDiagnostic::Outcome::Rejected: return "rejected";
    case TextToolCallDiagnostic::Outcome::IgnoredWithNative: return "ignored_with_native";
    case TextToolCallDiagnostic::Outcome::None: break;
    }
    return "none";
}

// 文本工具调用诊断 → JSON(trajectory payload / 被拒消息 metadata 共用)。
// raw_excerpt 只进这里(诊断用),绝不进发给模型的正文。
nlohmann::json text_tool_call_diagnostic_to_json(const TextToolCallDiagnostic& diag) {
    nlohmann::json out{
        {"outcome", text_tool_call_outcome_name(diag.outcome)},
        {"format", diag.format},
        {"reason", diag.reason},
        {"error", diag.error},
        {"tools", diag.attempted_tools},
        {"raw_excerpt", diag.raw_excerpt},
    };
    if (!diag.unexecuted_detail.empty()) out["unexecuted"] = diag.unexecuted_detail;
    if (diag.recovered_count > 0) out["count"] = diag.recovered_count;
    return out;
}

// 被拒文本工具调用的落盘正文:provider 已去掉的标记不再出现;可疑级(标记
// 已经流出)截到 visible_cut;去掉末尾空白,只剩空白时清成空串 —— 切断
// 「历史里越多文本调用样本、模型越模仿」的循环,headless 也不会把 "\n\n\n"
// 当成最终回复。
std::string text_tool_call_rejected_persisted_content(
    const std::string& content, const TextToolCallDiagnostic& diag) {
    std::string persisted = content;
    if (diag.visible_cut != std::string::npos && diag.visible_cut < persisted.size()) {
        persisted.resize(diag.visible_cut);
    }
    const auto last = persisted.find_last_not_of(" \t\r\n");
    if (last == std::string::npos) return {};
    persisted.resize(last + 1);
    return persisted;
}

std::string build_session_scratch_dir(const std::string& cwd,
                                      SessionManager* session_manager) {
    if (cwd.empty() || !session_manager) return {};
    const std::string session_id = session_manager->ensure_active_session_id();
    if (session_id.empty()) return {};
    return path_to_utf8(path_from_utf8(cwd) / ".acecode" / "tmp" /
                        ("session-" + session_id));
}

} // namespace acecode::agent::detail
