#pragma once

#include <string>

namespace acecode {

class MemoryRuntime;
class SessionManager;

struct MemoryCommandContext {
    MemoryRuntime* runtime = nullptr;   // nullable:记忆不可用
    SessionManager* session = nullptr;  // nullable:没有当前会话
    bool web = false;                   // 网页 / 桌面对话(edit 改为提示去设置页)
};

struct MemoryCommandResult {
    std::string text;
    // /memory edit 在 TUI 中要用 $EDITOR 打开的文件;为空表示无需打开。
    std::string edit_path;
};

// /memory 的唯一文本实现(openspec unify-memory-system D8):TUI 内置命令与 daemon
// 内置命令白名单都调用它,两端对同一输入产生相同文本(edit 除外)。子命令:
// list [--scope=global|workspace] [--type=<t>] / view <name> / edit <name> /
// forget <name> / flush / off / on / reload。
MemoryCommandResult dispatch_memory_command(const std::string& args,
                                            const MemoryCommandContext& ctx);

} // namespace acecode
