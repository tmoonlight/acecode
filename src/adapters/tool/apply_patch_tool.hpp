#pragma once

#include "tool_executor.hpp"

namespace acecode {

// Codex / GPT 系模型的编辑工具(openspec add-gpt-apply-patch-adaptation)。
// 始终注册;是否出现在模型侧工具表由 model_family::filter_tool_definitions_for_model
// 按当前模型决定。
ToolImpl create_apply_patch_tool();

} // namespace acecode
