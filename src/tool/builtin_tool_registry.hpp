#pragma once

#include "../config/config.hpp"
#include "agent_browser/browser_tools.hpp"
#include "bash_tool.hpp"
#include "file_edit_tool.hpp"
#include "file_read_tool.hpp"
#include "file_write_tool.hpp"
#include "glob_tool.hpp"
#include "goal_tool.hpp"
#include "image_generate/image_generate_tool.hpp"
#include "grep_tool.hpp"
#include "lsp_tool.hpp"
#include "plan_mode_tool.hpp"
#include "show_image_tool.hpp"
#include "task_complete_tool.hpp"
#include "todo_write_tool.hpp"
#include "tool_executor.hpp"
#include "vision_subagent_tool.hpp"
#include "web_search/runtime.hpp"
#include "web_search/web_search_tool.hpp"
#include "worktree_tool.hpp"

namespace acecode {

inline void register_session_builtin_tools(ToolExecutor& tools, const AppConfig& config) {
    tools.register_tool(create_bash_tool());
    tools.register_tool(create_file_read_tool());
    tools.register_tool(create_file_write_tool());
    tools.register_tool(create_file_edit_tool());
    tools.register_tool(create_show_image_tool());
    tools.register_tool(create_grep_tool());
    tools.register_tool(create_glob_tool());
    tools.register_tool(create_task_complete_tool());
    tools.register_tool(create_todo_write_tool());
    tools.register_tool(create_get_goal_tool());
    tools.register_tool(create_create_goal_tool());
    tools.register_tool(create_update_goal_tool());
    tools.register_tool(create_enter_plan_mode_tool());
    tools.register_tool(create_exit_plan_mode_tool());
    tools.register_tool(create_enter_worktree_tool(config.worktree));
    tools.register_tool(create_exit_worktree_tool());
    tools.register_tool(create_vision_analyze_tool(config));
    // 图像生成:端点配不出来就不注册 —— 注册一个必然失败的工具
    // 只会让模型反复调用反复失败。
    if (auto image_tool = create_image_generate_tool(config)) {
        tools.register_tool(*image_tool);
    }
    if (config.web_search.enabled) {
        tools.register_tool(web_search::create_web_search_tool(
            web_search::runtime().router(), web_search::runtime().cfg()));
    }
    if (config.lsp.enabled) {
        tools.register_tool(create_lsp_tool());
    }
    agent_browser::register_agent_browser_tools(tools);
}

} // namespace acecode
