#include "request_context_factory.hpp"
#include "agent/agent_loop.hpp"
#include "api_request_builder.hpp"
#include "prompt_context_cache.hpp"
#include "request_context.hpp"
#include "agent/model_step/active_model_view.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "prompt/prompt_environment.hpp"
#include "session/session_manager.hpp"

namespace acecode {

std::set<std::string> AgentLoop::dormant_skill_names() const {
    return agent::ApiRequestBuilder::dormant_skills(request_source_.skills, request_source_.skill_usage, request_source_.skill_idle_days);
}

agent::RequestContextOptions AgentLoop::request_context_options(
    const std::shared_ptr<LlmProvider>& provider, bool swarm_mode) const {
    agent::RequestContextFactory context(
        *boundary_, *exec_security_, request_source_, context_window_, session_manager_,
        tools_, permissions_, *history_, *request_builder_, *hooks_);
    return context.options(provider, swarm_mode);
}

std::vector<ChatMessage> AgentLoop::build_compaction_initial_context() const {
    const auto provider = provider_accessor_ ? provider_accessor_() : nullptr;
    return request_builder_->initial_context(request_context_options(provider));
}

AgentLoop::ApiRequestBundle AgentLoop::build_api_request_messages(
    const std::shared_ptr<LlmProvider>& provider, bool emergency_profile, bool swarm_mode) {
    agent::RequestContextFactory context(
        *boundary_, *exec_security_, request_source_, context_window_, session_manager_,
        tools_, permissions_, *history_, *request_builder_, *hooks_);
    return context.build(provider, emergency_profile, swarm_mode);
}
void AgentLoop::invalidate_git_snapshot() {
    prompt_cache_->invalidate_git();
}
} // namespace acecode
