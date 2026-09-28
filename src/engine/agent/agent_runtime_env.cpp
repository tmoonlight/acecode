#include "agent_runtime_env.hpp"
#include "pa/pa_context_budget.hpp"
#include "permissions/interaction_mode.hpp"
#include "prompt/prompt_environment.hpp"
#include "tool/mtime_tracker.hpp"
#include "utils/paths.hpp"

namespace acecode {
AgentRuntimeEnv::AgentRuntimeEnv()
    : context_budget([]() -> pa::ContextBudgetLearner& { return pa::context_budget(); }),
      mtime_tracker([]() -> MtimeTracker& { return MtimeTracker::instance(); }),
      computer_use_lease([](std::string owner, computer_use::SessionLease::Release release) {
          return std::make_unique<computer_use::SessionLease>(std::move(owner), std::move(release));
      }),
      computer_use_release(computer_use::release_session),
      prompt_environment(environment::prompt_environment),
      headless(headless::active), acecode_dir(get_acecode_dir) {}
} // namespace acecode
