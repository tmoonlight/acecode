#include "agent/agent_loop.hpp"
#include "user_shell_task.hpp"
#include "agent/transcript/trajectory_recorder.hpp"

namespace acecode {
void AgentLoop::run_shell(std::string command) {
    auto shell = std::make_unique<agent::UserShellTask>(
        tools_, *boundary_, abort_signal_, busy_, callbacks_, events_, *transcript_, *hooks_);
    shell->run(std::move(command), session_manager_, hook_manager_,
        trajectory_ ? trajectory_->ref() : LifetimeRef<agent::TrajectoryRecorder>{});
}
} // namespace acecode
