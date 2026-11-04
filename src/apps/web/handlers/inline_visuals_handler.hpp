#pragma once

// 「可视化图表」开关(设置 > 常规)的 REST 纯函数层:GET 快照与 PUT 解析。IO 与锁在
// routes_inline_visuals.cpp。开关只决定系统提示里有没有 visual 部分,见
// docs/inline-visuals.md。

#include "config/config.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace acecode::web {

// GET /api/config/inline-visuals 的响应体:{enabled}
nlohmann::json inline_visuals_snapshot(const InlineVisualsConfig& cfg);

// 解析 PUT body。语义是 patch:缺省键沿用 current;enabled 出现时必须是布尔。
// 失败时 out 不变、error 是给用户看的文案。
bool parse_inline_visuals_request(
    const nlohmann::json& body,
    const InlineVisualsConfig& current,
    InlineVisualsConfig& out,
    std::string& error);

} // namespace acecode::web
