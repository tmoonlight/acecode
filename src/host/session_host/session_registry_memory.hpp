#pragma once

#include "session/session_client.hpp"

namespace acecode {

class MemoryRuntime;
struct SessionEntry;

// daemon 内置命令 /memory:与 TUI 同一份 dispatch_memory_command 文本,经会话
// system message 透出到网页 / 桌面对话(edit 改为提示去设置页)。
BuiltinCommandResult execute_memory_builtin(SessionEntry& entry,
                                            const BuiltinCommandRequest& request,
                                            MemoryRuntime* runtime);

} // namespace acecode
