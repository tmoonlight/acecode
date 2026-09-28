#include "agent/agent_loop.hpp"
#include "agent/approval/session_exec_security.hpp"
#include "agent/boundary/workspace_boundary.hpp"
#include "prompt/system_prompt.hpp"

#include <utility>

namespace acecode {

std::string AgentLoop::cwd() const { return boundary_->cwd(); }
void AgentLoop::set_loop_execution_policy(LoopExecutionPolicy policy) {
    request_source_.loop = std::move(policy);
    boundary_->set_loop_active(request_source_.loop.active);
}
void AgentLoop::set_inherited_write_root(std::string root) {
    boundary_->set_inherited_write_root(std::move(root));
}
void AgentLoop::refresh_workspace_folders() {
    boundary_->refresh_workspace_folders(session_manager_);
    exec_security_->runtime().set_workspace_writable_roots(writable_workspace_folders());
}
std::vector<std::string> AgentLoop::workspace_extra_folders() const {
    return boundary_->workspace_extra_folders();
}
std::vector<std::string> AgentLoop::writable_workspace_folders() const {
    return boundary_->writable_workspace_folders(session_manager_);
}
std::string AgentLoop::write_root() const {
    return boundary_->write_root(session_manager_);
}

} // namespace acecode
