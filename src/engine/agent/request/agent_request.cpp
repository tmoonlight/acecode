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
    agent::RequestContextSource source;
    { std::lock_guard<std::mutex> lock(request_source_mu_); source = request_source_; }
    return agent::ApiRequestBuilder::dormant_skills(source.skills.get(), source.skill_usage, source.skill_idle_days);
}

agent::RequestContextOptions AgentLoop::request_context_options(
    const std::shared_ptr<LlmProvider>& provider) const {
    const auto source = capture_request_source();
    agent::RequestContextFactory context(
        *boundary_, *exec_security_, source, context_window_, session_manager_,
        tools_, permissions_, *history_, *request_builder_, *hooks_);
    return context.options(provider);
}

agent::RequestContextSource AgentLoop::capture_request_source() const {
    agent::RequestContextSource snapshot;
    {
        std::lock_guard<std::mutex> lock(request_source_mu_);
        snapshot = request_source_;
    }
    // The provider may acquire the host AppConfig lock. Never call it while
    // holding this loop's publication mutex.
    if (prompt_config_provider_) snapshot.prompt_config = prompt_config_provider_();
    if (session_manager_) {
        // 蜂群模式与网状路径以会话元数据为准(/swarm、消息接口、resume 只改它)。
        const SwarmMode mode =
            parse_swarm_mode(session_manager_->current_swarm_mode()).value_or(SwarmMode::Off);
        const std::string path = session_manager_->current_agent_path();
        snapshot.swarm.mode = mode;
        snapshot.swarm.agent_path =
            mode == SwarmMode::Mesh ? (path.empty() ? std::string("/root") : path) : std::string{};
        snapshot.swarm.mesh.is_root = snapshot.swarm.agent_path == "/root";
    }
    // 蜂群模式两套协作工具互斥:按本回合捕获的模式隐藏另一套(schema 与执行同一谓词)。
    snapshot.tool_policy.hidden_builtin_tools = swarm_mode_hidden_tools(snapshot.swarm.mode);
    return snapshot;
}

void AgentLoop::publish_skill_snapshot(std::shared_ptr<const SkillRegistry> skills) {
    std::lock_guard<std::mutex> lock(request_source_mu_);
    request_source_.skills = std::move(skills);
}

void AgentLoop::publish_expert_snapshot(std::shared_ptr<const ExpertDefinition> expert,
    std::shared_ptr<const SkillRegistry> skills, ToolCapabilityPolicy policy, std::string member_id) {
    std::lock_guard<std::mutex> lock(request_source_mu_);
    request_source_.expert = std::move(expert);
    request_source_.skills = std::move(skills);
    request_source_.tool_policy = std::move(policy);
    request_source_.expert_member = std::move(member_id);
}

std::vector<ChatMessage> AgentLoop::build_compaction_initial_context() const {
    const auto provider = provider_accessor_ ? provider_accessor_() : nullptr;
    return request_builder_->initial_context(request_context_options(provider));
}

void AgentLoop::invalidate_git_snapshot() {
    prompt_cache_->invalidate_git();
}
} // namespace acecode
