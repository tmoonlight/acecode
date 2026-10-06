#pragma once

// 跨工作区的会话目录:活跃会话 + 已登记工作区 + 无项目会话,排除归档与子代理会话。
// /rc 远程控制与消息通道(QQ / Telegram)的 /sessions 共用这一份目录。

#include "remote_control/rc_session_navigation.hpp"
#include "session/session_client.hpp"

#include <optional>
#include <string>
#include <vector>

namespace acecode::daemon {

std::vector<rc::RcSessionTarget> build_rc_session_catalog(const std::string& projects_dir,
                                                          SessionClient& client,
                                                          const std::optional<std::string>& query);

} // namespace acecode::daemon
