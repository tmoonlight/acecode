#pragma once

#include "tool_executor.hpp"

namespace acecode {

// 停止本会话的一个后台任务(后台命令杀掉整棵进程树,后台子代理中止当前回合)。
// 不传 task_id 时列出还在跑的后台任务。
ToolImpl create_stop_background_task_tool();

} // namespace acecode
