#pragma once

// /swarm [star|mesh|off] — 切换当前会话的蜂群模式(add-mesh-swarm-mode)。
//
// 文本解析与输出与 daemon builtin 共用 session_host/swarm_command.hpp;
// TUI 主会话不在 SessionRegistry 里,模式直接写主会话的 SessionManager,
// 下一回合生效。退出网状模式前由 agent 树服务判定是否仍有子 agent 在跑。

#include "command_registry.hpp"

namespace acecode {

void register_swarm_mode_command(CommandRegistry& registry);

} // namespace acecode
