#pragma once

#include "tool/tool_executor.hpp"
#include "tool/tool_rewrites.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace acecode::web {

// GET /api/config/tool-rewrites 的响应体。tools 只列 Builtin 来源、按名字排序,
// 这就是设置页左列「所有内置工具」的数据源;defaults 是内置种子,前端用来
// 提供「恢复默认」。
nlohmann::json tool_rewrites_snapshot(
    const tool_rewrites::ToolRewriteSettings& settings,
    const std::vector<RegisteredToolInfo>& registered_tools,
    const std::string& path,
    const std::string& load_warning = {});

// 解析 PUT body `{enabled, rewrites:{native:public}}`(整体替换,不是 patch)。
// 结构校验 + 与注册表撞名校验;失败时 out 不变、error 是给用户看的文案。
bool parse_tool_rewrites_request(
    const nlohmann::json& body,
    const std::vector<RegisteredToolInfo>& registered_tools,
    tool_rewrites::ToolRewriteSettings& out,
    std::string& error);

} // namespace acecode::web
