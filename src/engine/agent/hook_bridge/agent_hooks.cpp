#include "agent/agent_loop.hpp"
#include "agent/hook_bridge/agent_hook_bridge.hpp"

namespace acecode {

void AgentLoop::dispatch_session_start_hook(const std::string& source) {
    hooks_->session_start(hook_manager_, session_manager_, source);
}
void AgentLoop::dispatch_session_title_changed_hook(
    const std::string& title, const std::string& source, const std::string& title_source) {
    hooks_->session_title_changed(hook_manager_, session_manager_, title, source, title_source);
}

} // namespace acecode
