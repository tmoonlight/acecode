#pragma once
#include "agent/agent_callbacks.hpp"
#include "agent/agent_runtime_env.hpp"
#include "agent/request/request_context_source.hpp"
#include "config/config.hpp"
#include "security/audit_log.hpp"
#include <functional>
#include <memory>
#include <string>

namespace acecode {
class LlmProvider; class PermissionManager; class SessionManager; class HookManager;
class SkillUsageStore; class MemoryService; class SkillRegistry; struct ExpertDefinition;
using AgentProviderAccessor = std::function<std::shared_ptr<LlmProvider>()>;

struct AgentLoopServices {
    ToolExecutor& tools;
    PermissionManager& permissions;
    AgentProviderAccessor provider;
    AgentCallbacks callbacks;
    SessionManager* session = nullptr; // Nullable borrowed; outlives the loop.
    HookManager* hooks = nullptr; // Nullable borrowed; outlives the loop.
    SkillUsageStore* skill_usage = nullptr; // Nullable borrowed; outlives the loop.
    // 记忆服务(全局 + 会话工作区作用域);三个入口与子会话共用同一个实例。
    std::shared_ptr<MemoryService> memory;
    // Shared immutable policy snapshots retained by the loop and its caller.
    std::shared_ptr<const SkillRegistry> skills;
    std::shared_ptr<const ExpertDefinition> expert;
    security::AuditSink audit_sink;
    AgentRuntimeEnv runtime;
    PromptConfigProvider prompt_config;
};
struct AgentLoopOptions {
    std::string cwd;
    AgentLoopConfig config;
    int context_window = 128000;
    std::string no_model_config_prompt;
    int task_suggestion_compact_threshold = 3;
    LoopExecutionPolicy loop_policy;
    std::string inherited_write_root;
    ToolCapabilityPolicy tool_policy;
    std::string expert_member_id;
    int skill_idle_days = 30;
    std::optional<SandboxConfig> sandbox; // Absent preserves the unconfigured runtime.
    std::string exec_rules_dir_override;
};
} // namespace acecode
