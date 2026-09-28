#pragma once

#include "llm/llm_provider.hpp"
#include "prompt/context_usage_breakdown.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace acecode::agent {

using ProgressEmitter = std::function<void(
    const std::string& phase, const std::string& label,
    const std::string& detail, const std::string& tool,
    const std::string& tool_call_id, int tool_index, bool force)>;

struct UserTurnInfo {
    ChatMessage user_msg;
    bool visible_timed_turn = false;
    std::string turn_user_uuid;
    std::string active_turn_id;
    std::int64_t turn_started_at_ms = 0;
};

struct ApiRequestBundle {
    std::vector<ChatMessage> messages_with_system;
    std::vector<ToolDef> tool_defs;
    ContextUsageBreakdown context_usage_estimate;
    nlohmann::json prompt_diag; // simplified: store as raw json
};

// Wire metadata key, independent of progress phrase generation.
inline constexpr const char* kToolPreambleMetadataKey = "tool_preamble";

struct ToolPreambleTitle {
    std::string title;
    std::string source;   // reasoning | template | context
    std::string kind;     // read | write | ""(按工具类型定,透传给界面)
};

struct ProviderCallResult {
    ChatResponse accumulated;
    bool provider_error_seen = false;
    ProviderErrorInfo provider_error_info;
    std::shared_ptr<LlmProvider> provider_snapshot;
    int provider_attempt = 1;
};

struct WorkerTask {
    enum class Kind { Chat, Shell, Compact, Control };
    Kind kind = Kind::Chat;
    std::string payload;
    UserInput input;
    // 仅 Chat 用:UI 渲染时希望显示的"原文",而 payload(发给 LLM)可能
    // 是被 daemon expander 展开过的字符串(skill 调用提示等)。空 = UI 与
    // LLM 看到同一份(payload)。
    std::string display_text;
    bool hidden_goal_context = false;
    std::function<void()> control;
    std::string retry_user_message_id;
};

enum class HandleErrorResult { Continue, Break, Proceed };

enum class ContextRecoveryStage {
    Normal,
    HistoryRepaired,
    EmergencyProfile,
};

} // namespace acecode::agent
