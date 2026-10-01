#pragma once

#include "tool_executor.hpp"

#include <string>

namespace acecode {

// 记忆工具按「会话」解析作用域(D2):工作区作用域 = 会话项目目录下的 memory/,
// 不看进程 cwd —— daemon 一个进程服务多个工作区。子会话与父会话的项目目录相同,
// 因此自然共享父会话的工作区作用域。
struct MemoryToolScope {
    std::string project_dir;   // 空 = 没有工作区(只有全局作用域)
    std::string session_id;
};

MemoryToolScope resolve_memory_tool_scope(const ToolContext& ctx);

} // namespace acecode
