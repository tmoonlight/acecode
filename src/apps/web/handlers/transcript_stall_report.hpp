#pragma once

// 前端「实时状态卡住」诊断上报(POST /api/diagnostics/transcript-stall)的纯函数层。
// 页面发现会话显示仍在运行、却很久没收到事件,或 WS 心跳超时、事件游标被服务端
// 重置时,把当时的游标与连接状态报上来,daemon 记一行 WARN 日志。这里只做白名单
// 校验:未知字段一律丢弃,字符串限字符集与长度,数值必须有限且非负 —— 客户端
// 提交的任何内容都不能原样进日志。路由与日志写入在 routes_diagnostics.cpp。

#include <nlohmann/json.hpp>

#include <optional>

namespace acecode::web {

// 校验通过返回只含白名单字段的 JSON(kind 必填,其余可选);任一字段类型或取值
// 不合法返回 nullopt,调用方回 400。
std::optional<nlohmann::json> sanitize_transcript_stall_report(const nlohmann::json& body);

} // namespace acecode::web
