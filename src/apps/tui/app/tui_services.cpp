#include "tui/app/tui_services.hpp"
#include "tui/app/tui_runtime_init.hpp"
#include "cli/interactive_options.hpp"
#include "hooks/hook_manager.hpp"
#include "hooks/hook_payload.hpp"
#include "agent/hook_bridge/hook_events.hpp"
#include "provider/cwd_model_override.hpp"
#include "tool/tool_executor.hpp"
#include "tool/tool_rewrites.hpp"
#include "tool/mcp_manager.hpp"
#include "tool/workspace_tools.hpp"
#include "security/audit_log.hpp"
#include "skills/skill_registry.hpp"
#include "skills/skill_usage_store.hpp"
#include "memory/memory_registry.hpp"
#include "workspace/workspace_registry.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"
#include <iostream>
namespace acecode::tui {
TuiServices::TuiServices() = default;
TuiServices::~TuiServices() = default;
bool TuiServices::initialize(const InteractiveCliOptions& cli, const std::string& cwd,
    const std::string& argv0_dir) {
    hooks = std::make_unique<HookManager>(load_tui_hook_config());
    {
        auto payload = build_startup_before_model_load_payload(cwd);
        hooks->dispatch(kHookEventStartupBeforeModelLoad, payload, cwd);
    }
    config = load_tui_config_and_runtime(*hooks, cwd, argv0_dir);
    if (!cli.question_policy_error.empty()) {
        std::cerr << "[acecode] " << cli.question_policy_error << std::endl;
        return false;
    }
    if (!cli.question_policy.empty()) {
        config.agent_loop.question_policy_cli = cli.question_policy;
        config.agent_loop.question_timeout_seconds_cli = cli.question_timeout_seconds;
    }
    cwd_model_override = load_cwd_model_override(cwd);
    initial_model = initialize_tui_provider_runtime(config, cwd, cwd_model_override, model_binding, *hooks);
    tool_rewrites::load_and_apply(get_acecode_dir());
    security::audit_log().configure(get_acecode_dir());
    tools = std::make_unique<ToolExecutor>();
    skills = std::make_unique<SkillRegistry>();
    memory = std::make_unique<MemoryRegistry>();
    mcp = std::make_unique<McpManager>();
    runtime_memory_config = initialize_tui_tools_and_registries(
        *tools, *skills, *memory, *mcp, config, cwd);
    const auto projects = path_to_utf8(path_from_utf8(get_acecode_dir()) / "projects");
    workspaces = std::make_unique<desktop::WorkspaceRegistry>();
    workspaces->scan(projects);
    workspace_tool_deps = std::make_shared<WorkspaceToolDeps>();
    workspace_tool_deps->registry = workspaces.get();
    workspace_tool_deps->projects_dir = projects;
    register_workspace_tools(*tools, workspace_tool_deps);
    skill_usage = std::make_shared<SkillUsageStore>(get_acecode_dir() + "/.skill_usage_state.json");
    return true;
}
}
