#pragma once

// system prompt # Environment 段的快照来源:终端运行时 + 已注入的工具链目录。
// AgentLoop 每次构建 system prompt 时取一份传给 build_system_prompt,避免把
// AppConfig 整个穿进 AgentLoop。两个来源都只在配置变化时变,满足 prompt cache
// 前缀不变量。

#include "prompt/system_prompt.hpp"

namespace acecode::environment {

SystemPromptEnvironment prompt_environment();

}  // namespace acecode::environment
