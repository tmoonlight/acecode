#pragma once

// 消息通道状态日志(openspec add-desktop-im-channels,2026-10-05 验收反馈):设置页只显示
// 简要状态,细节——连接状态与原因、平台附加信息(隐私模式、webhook、暂存待补发)、机主、
// 授权名单、待批准请求、会话绑定与收发计数——在平台快照变化时写进 daemon 日志。
// 只用快照里本就可以展示的字段;凭据(包括脱敏尾号)与二维码内容一律不写。

#include "utils/logger.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace acecode::channels::core {

struct StateLogLine {
    LogLevel level = LogLevel::Info;
    std::string text;
};

// 比较同一平台前后两份快照(PlatformRuntime::snapshot 的结构),返回要写的日志行,
// 不含 "[channels/<平台>] " 前缀。previous 不是对象时视为首次发布,输出当前状态的摘要;
// 两份内容相同时返回空。
std::vector<StateLogLine> describe_state_changes(const nlohmann::json& previous, const nlohmann::json& next);

} // namespace acecode::channels::core
