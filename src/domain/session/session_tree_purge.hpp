#pragma once

// 子会话(spawn_subagent 后台任务、网状 agent)没有独立的生命周期:不能单独
// 归档或删除,只随主会话一起永久删除,否则事后分析会缺上下文(用户决策)。
// Web 永久删除、TUI 设置中心与线程工具都按这里的顺序删。

#include <string>
#include <vector>

namespace acecode {

// root_id 的全部后代会话(按 parent_session_id 递归)在前、root_id 本身在最后。
// 只读磁盘 meta;还没落盘、只在内存里的子会话由调用方自己补。
std::vector<std::string> session_tree_delete_order(const std::string& project_dir,
                                                   const std::string& root_id);

// 按 session_tree_delete_order 逐个删除用户消息搜索索引与磁盘数据(jsonl、meta、
// <id>/ 目录)。中途失败即停并返回 false:主会话最后删,失败时仍可重试。
bool purge_session_tree(const std::string& project_dir,
                        const std::string& root_id,
                        std::string* error = nullptr);

}  // namespace acecode
