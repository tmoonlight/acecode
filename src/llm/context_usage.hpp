#pragma once

// ContextUsageBreakdown 的 JSON 编解码(P2-02 自 prompt/context_usage_breakdown 拆出):
// 会话落盘(session_storage)只需要往返 JSON,不该依赖 engine 层的 prompt 模块;估算与
// 按 provider 用量校准仍在 prompt/context_usage_breakdown。

#include "llm/llm_provider.hpp"

#include <nlohmann/json_fwd.hpp>

namespace acecode {

nlohmann::json context_usage_breakdown_to_json(
    const ContextUsageBreakdown& breakdown);
ContextUsageBreakdown context_usage_breakdown_from_json(
    const nlohmann::json& value);

} // namespace acecode
