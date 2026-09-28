#include "agent/agent_loop.hpp"
#include "agent/hook_bridge/agent_hook_bridge.hpp"

namespace acecode {

void AgentLoop::dispatch_assistant_completed_hook(
    const ChatMessage& message, const std::shared_ptr<LlmProvider>& provider) {
    hooks_->assistant_completed(hook_manager_, session_manager_, message, provider);
}
HookCommonPayloadFields AgentLoop::build_hook_common_fields(const std::string& event) const {
    return hooks_->common_fields(event, session_manager_);
}
void AgentLoop::apply_hook_side_effects(const HookAggregateOutcome& outcome, bool include_context) {
    hooks_->apply(outcome, include_context);
}
std::string AgentLoop::drain_hook_request_context() { return hooks_->drain_context(); }
HookAggregateOutcome AgentLoop::dispatch_codex_hook(
    const std::string& event, const std::string& matcher, const nlohmann::json& payload) {
    return hooks_->dispatch(hook_manager_, event, matcher, payload);
}
void AgentLoop::dispatch_session_start_hook(const std::string& source) {
    hooks_->session_start(hook_manager_, session_manager_, source);
}
void AgentLoop::dispatch_session_title_changed_hook(
    const std::string& title, const std::string& source, const std::string& title_source) {
    hooks_->session_title_changed(hook_manager_, session_manager_, title, source, title_source);
}

} // namespace acecode
