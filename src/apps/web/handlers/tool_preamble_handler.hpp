#pragma once

// 「具体进度提示」开关(设置 > 常规 > 工作模式:适合日常工作 = 开,openspec
// add-tool-preamble)的 REST 纯函数层:GET 快照与 PUT 解析。IO / 锁 / 下发在
// routes_tool_preamble.cpp。

#include "config/config.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace acecode::web {

// GET /api/config/tool-preamble 的响应体:{enabled}
nlohmann::json tool_preamble_snapshot(const ToolPreambleConfig& cfg);

// 解析 PUT body。语义是 patch:缺省键沿用 current;enabled 出现时必须是布尔。
// 旧客户端还会带 mode(提示驱动 / 推理摘要两版的遗留),忽略不报错。失败时 out
// 不变、error 是给用户看的文案。
bool parse_tool_preamble_request(
    const nlohmann::json& body,
    const ToolPreambleConfig& current,
    ToolPreambleConfig& out,
    std::string& error);

} // namespace acecode::web
