#pragma once

// /lsp slash 命令 — 查看 LSP 集成状态(openspec add-lsp-service)。
//
//   /lsp    显示 enabled 状态、已连接 server(root / 打开文件数)、
//           broken 列表、内置但未探测到可执行文件的 server。
//
// TUI 经 register_lsp_command 注册;daemon builtin(session_registry)与
// 这里共用 dispatch_lsp_subcommand 产出同一份文本。

#include "command_registry.hpp"

namespace acecode {

void register_lsp_command(CommandRegistry& registry);

} // namespace acecode
