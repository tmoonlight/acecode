#pragma once
#include "config/config.hpp"
#include "provider/session_model_binding.hpp"
#include <memory>
#include <optional>
#include <string>
namespace acecode {
struct InteractiveCliOptions;
class HookManager; class ToolExecutor; class SkillRegistry; class MemoryRegistry;
class McpManager; class SkillUsageStore; struct WorkspaceToolDeps;
namespace desktop { class WorkspaceRegistry; }
}
namespace acecode::tui {
class TuiServices {
public:
    TuiServices();
    ~TuiServices();
    TuiServices(const TuiServices&) = delete;
    TuiServices& operator=(const TuiServices&) = delete;
    bool initialize(const InteractiveCliOptions& cli, const std::string& cwd, const std::string& argv0_dir);
    // Declaration order preserves lifetime of dependencies retained by tools.
    std::unique_ptr<HookManager> hooks;
    AppConfig config;
    SessionModelBinding model_binding;
    std::unique_ptr<ToolExecutor> tools;
    std::unique_ptr<SkillRegistry> skills;
    std::unique_ptr<MemoryRegistry> memory;
    std::unique_ptr<McpManager> mcp;
    std::unique_ptr<desktop::WorkspaceRegistry> workspaces;
    // Shared by workspace tool closures and this service owner.
    std::shared_ptr<WorkspaceToolDeps> workspace_tool_deps;
    // Shared with UI state and session skill callbacks, persisted at one store.
    std::shared_ptr<SkillUsageStore> skill_usage;
    MemoryConfig runtime_memory_config;
    std::optional<std::string> cwd_model_override;
    ModelProfile initial_model;
};
}
