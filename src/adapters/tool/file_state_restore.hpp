#pragma once

// resume 时按历史里的文件工具调用恢复 MtimeTracker 基线(P2-08 自 tui/resume/session_resume_restore 拆出):
// TUI 的 --resume / /resume 与 daemon 的 SessionRegistry::resume 共用,宿主层不该为它依赖 TUI 目录。

#include "llm/llm_provider.hpp"

#include <string>
#include <vector>

namespace acecode {

// cwd:会话工作目录,apply_patch 历史调用里的相对路径按它解析后补
// MtimeTracker 基线;空串 = 相对路径原样(只对绝对路径生效)。
void restore_file_tool_state_from_messages(const std::vector<ChatMessage>& messages,
                                           const std::string& cwd = std::string());

} // namespace acecode
