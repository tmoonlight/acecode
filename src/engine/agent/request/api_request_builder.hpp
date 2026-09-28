#pragma once

#include "agent/turn/turn_types.hpp"
#include "config/config.hpp"
#include "experts/expert_registry.hpp"
#include "prompt/system_prompt.hpp"
#include "session/todo_state.hpp"
#include <optional>
#include <set>

namespace acecode { class SkillUsageStore; }

namespace acecode::agent {
class PromptContextCache;

// The skill snapshot is retained for this capture/initial_context operation.
// MemoryRegistry is a fixed nullable borrowed service that outlives the loop.
struct RequestContextOptions {
    std::string cwd;
    std::shared_ptr<const SkillRegistry> skills;
    const MemoryRegistry* memory = nullptr;
    std::optional<MemoryConfig> memory_config;
    std::optional<ProjectInstructionsConfig> project_config;
    std::optional<CustomInstructionsConfig> custom_config;
    std::optional<GitContextConfig> git_config;
    std::optional<ExpertDefinition> expert;
    std::string expert_member;
    ToolCapabilityPolicy tool_policy;
    int context_window = 128000;
    bool can_read_images = true;
    bool loop_active = false;
    bool swarm_mode = false;
    std::string loop_context;
    SystemPromptWorktreeState worktree;
    SystemPromptEnvironment environment;
    SystemPromptSandboxState sandbox;
    SystemPromptModelState model;
    SystemPromptWorkspaceFolders folders;
    SkillUsageStore* skill_usage = nullptr; // nullable, borrowed for this call
    int skill_idle_days = 30;
};

// Every mutable request input is a value. Hook draining and session queries
// happen before build; the builder cannot consume a loop-owned queue.
struct RequestBuildInputs {
    bool emergency_profile = false;
    std::string system_prompt;
    std::vector<ChatMessage> history;
    std::vector<ToolDef> tool_defs;
    std::vector<ToolDef> builtin_tool_defs;
    std::vector<ToolDef> mcp_tool_defs;
    PromptContextBlock skills;
    PromptContextBlock session;
    PromptContextCategoryBytes category_bytes;
    std::string swarm_context;
    std::string hook_context;
    std::string plan_context;
    std::vector<TodoItem> todos;
};

class ApiRequestBuilder {
public:
    ApiRequestBuilder(ToolExecutor& tools, PromptContextCache& cache)
        : tools_(tools), cache_(cache) {}
    RequestBuildInputs capture(const RequestContextOptions& options,
                               std::vector<ChatMessage> history, bool emergency_profile);
    ApiRequestBundle build(RequestBuildInputs inputs);
    std::vector<ChatMessage> initial_context(const RequestContextOptions& options) const;
    std::string static_system_prompt(const RequestContextOptions& options) const;
    static std::set<std::string> dormant_skills(const SkillRegistry* registry,
                                               SkillUsageStore* store, int idle_days);
private:
    ToolExecutor& tools_;
    PromptContextCache& cache_;
};

} // namespace acecode::agent
