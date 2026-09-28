#pragma once
#include "tui/render_mode.hpp"
#include <optional>
#include <string>
namespace acecode {
struct HookConfig;
class HookManager;
class SessionModelBinding;
class ToolExecutor;
class SkillRegistry;
class MemoryRegistry;
class McpManager;
}
namespace acecode::tui {
HookConfig load_tui_hook_config();
AppConfig load_tui_config_and_runtime(HookManager& hooks, const std::string& working_dir,
    const std::string& argv0_dir);
ModelProfile initialize_tui_provider_runtime(AppConfig& config, const std::string& working_dir,
    const std::optional<std::string>& cwd_override, SessionModelBinding& binding,
    HookManager& hooks);
MemoryConfig initialize_tui_tools_and_registries(ToolExecutor& tools, SkillRegistry& skills,
    MemoryRegistry& memory, McpManager& mcp, const AppConfig& config,
    const std::string& working_dir);
ScreenRenderMode initialize_tui_render_mode(AppConfig& config, bool force_alt_screen,
    TerminalCapabilities& capabilities, bool& conhost_compat_layout);
}
