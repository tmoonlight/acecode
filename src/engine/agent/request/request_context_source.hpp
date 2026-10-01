#pragma once
#include "tool/tool_executor.hpp"
#include "agent/agent_runtime_env.hpp"
#include "session_prompt_config.hpp"
#include <memory>
#include <string>

namespace acecode {
class MemoryService;
class SkillUsageStore;
struct MemoryConfig;
struct ProjectInstructionsConfig;
struct CustomInstructionsConfig;
struct GitContextConfig;
struct ExpertDefinition;

struct LoopExecutionPolicy {
    bool active = false;
    std::string system_context;
};
}
namespace acecode::agent {

// Fixed services plus owned configuration/capability snapshots. A turn copies
// this value once, and every request/compaction within that turn retains it.
struct RequestContextSource {
    AgentRuntimeEnv runtime;
    std::shared_ptr<const SkillRegistry> skills;
    SkillUsageStore* skill_usage = nullptr; // Nullable borrowed session service.
    int skill_idle_days = 30;
    std::shared_ptr<MemoryService> memory; // Nullable shared session service.
    SessionPromptConfig prompt_config;
    std::shared_ptr<const ExpertDefinition> expert;
    std::string expert_member;
    ToolCapabilityPolicy tool_policy;
    LoopExecutionPolicy loop;
};
} // namespace acecode::agent
