#pragma once

// Shared resume restoration glue.
//
// `session_replay` handles the pure canonical-message -> TUI-row expansion.
// This helper handles the integration concerns that both CLI `--resume` and
// slash `/resume` need: rebuild the AgentLoop's canonical message history,
// preserve shell-mode `!cmd + tool_result` pairs as injected shell turns, and
// append replayed TUI rows into TuiState.

#include "llm/llm_provider.hpp"

#include <string>
#include <vector>

namespace acecode {

class AgentLoop;
class ToolExecutor;
struct TuiState;

// cwd:会话工作目录,apply_patch 历史调用里的相对路径按它解析后补
// MtimeTracker 基线;空串 = 相对路径原样(只对绝对路径生效)。
void restore_file_tool_state_from_messages(const std::vector<ChatMessage>& messages,
                                           const std::string& cwd = std::string());

void append_resumed_session_messages(const std::vector<ChatMessage>& messages,
                                     TuiState& state,
                                     AgentLoop& agent_loop,
                                     const ToolExecutor& tools);

} // namespace acecode
