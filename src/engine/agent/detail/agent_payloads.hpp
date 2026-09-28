#pragma once

#include "llm/llm_provider.hpp"
#include "provider/text_tool_call_recovery.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace acecode { class SessionManager; }

namespace acecode::agent::detail {

inline constexpr const char* kDefaultNoModelConfiguredPrompt =
    u8"请先配置大模型服务。";

std::string build_session_scratch_dir(const std::string& cwd, SessionManager* session_manager);

nlohmann::json build_agent_progress_payload(
    const std::string& phase,
    const std::string& label,
    const std::string& detail,
    const std::string& tool,
    const std::string& tool_call_id,
    int tool_index,
    std::int64_t started_at_ms);

nlohmann::json model_step_usage_to_json(const TokenUsage& usage);

void accumulate_turn_usage(TokenUsage& aggregate,
                           bool& initialized,
                           const TokenUsage& step);

const char* text_tool_call_outcome_name(TextToolCallDiagnostic::Outcome outcome);

nlohmann::json text_tool_call_diagnostic_to_json(const TextToolCallDiagnostic& diag);

std::string text_tool_call_rejected_persisted_content(
    const std::string& content, const TextToolCallDiagnostic& diag);

} // namespace acecode::agent::detail
