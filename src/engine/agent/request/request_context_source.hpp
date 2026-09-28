#pragma once
#include "tool/tool_executor.hpp"
#include "agent/agent_runtime_env.hpp"
#include <string>

namespace acecode {
class MemoryRegistry;
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

// Prompt inputs formerly stored separately in AgentLoop. Existing setup/control
// writers remain unchanged in A-13. O-10 replaces the four borrowed config
// fields with the explicit per-turn SessionPromptConfig publication contract.
struct RequestContextSource {
    AgentRuntimeEnv runtime;
    const SkillRegistry* skills = nullptr; // Nullable borrowed session service.
    SkillUsageStore* skill_usage = nullptr; // Nullable borrowed session service.
    int skill_idle_days = 30;
    const MemoryRegistry* memory = nullptr; // Nullable borrowed session service.
    const MemoryConfig* memory_config = nullptr; // Legacy nullable borrow; O-10.
    const ProjectInstructionsConfig* project_config = nullptr; // Legacy nullable borrow; O-10.
    const CustomInstructionsConfig* custom_config = nullptr; // Legacy nullable borrow; O-10.
    const GitContextConfig* git_config = nullptr; // Legacy nullable borrow; O-10.
    const ExpertDefinition* expert = nullptr; // Legacy nullable borrow; O-10.
    std::string expert_member;
    ToolCapabilityPolicy tool_policy;
    LoopExecutionPolicy loop;
};
} // namespace acecode::agent
