#pragma once

#include "lsp_service.hpp"

#include <string>

namespace acecode {

// 纯函数:状态快照 → 用户可读多行文本(单测覆盖)。
std::string format_lsp_status(const lsp::LspService::Status& status);

// 子命令分发。runtime 未初始化时返回提示文本,不抛。
std::string dispatch_lsp_subcommand(const std::string& sub);

} // namespace acecode
