#pragma once

#include "agent/turn/turn_types.hpp"
#include "config/config.hpp"
#include "experts/expert_registry.hpp"
#include "prompt/system_prompt.hpp"
#include "session/todo_state.hpp"
#include <optional>
#include <set>

namespace acecode { class SkillUsageStore; class MemoryService; }

namespace acecode::agent {
class PromptContextCache;

// The skill snapshot is retained for this capture/initial_context operation.
// MemoryService is a nullable borrowed service that outlives this call.
struct RequestContextOptions {
    std::string cwd;
    std::shared_ptr<const SkillRegistry> skills;
    MemoryService* memory = nullptr;
    // 生效的记忆配置;本会话 /memory off 时 enabled=false(不注入、不给记忆工具)。
    std::optional<MemoryConfig> memory_config;
    std::string memory_project_dir;  // 会话项目目录;空 = 没有工作区,只有全局记忆
    std::string memory_session_key;  // 会话 id;切换 / 恢复会话时记忆快照随之重建
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
    static bool memory_active(const RequestContextOptions& options);
    static PromptContextBlock render_memory_snapshot(const RequestContextOptions& options);
    // 压缩 / 线程修复后调用:下一次请求按磁盘重建记忆快照。
    void invalidate_memory_snapshot();
private:
    const PromptContextBlock& frozen_memory_snapshot(const RequestContextOptions& options);
    ToolExecutor& tools_;
    PromptContextCache& cache_;
};

} // namespace acecode::agent
