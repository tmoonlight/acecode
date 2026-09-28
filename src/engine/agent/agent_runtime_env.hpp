#pragma once
#include "prompt/system_prompt.hpp"
#include "computer_use/session_lease.hpp"
#include <functional>
#include <memory>
#include <string>

namespace acecode {
class MtimeTracker;
namespace pa { class ContextBudgetLearner; }
// Process services remain process-scoped. Tests can replace each access point;
// callbacks return a service lease/reference valid for the synchronous use.
struct AgentRuntimeEnv {
    AgentRuntimeEnv();
    std::function<pa::ContextBudgetLearner&()> context_budget;
    std::function<MtimeTracker&()> mtime_tracker;
    std::function<std::unique_ptr<computer_use::SessionLease>(
        std::string, computer_use::SessionLease::Release)> computer_use_lease;
    std::function<void(const std::string&)> computer_use_release;
    std::function<SystemPromptEnvironment()> prompt_environment;
    std::function<bool()> headless;
    std::function<std::string()> acecode_dir;
};
} // namespace acecode
