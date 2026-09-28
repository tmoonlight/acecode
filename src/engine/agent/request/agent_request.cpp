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
    return agent::ApiRequestBuilder::dormant_skills(skill_registry_, skill_usage_store_, skill_idle_days_);
}

agent::RequestContextOptions AgentLoop::request_context_options(
    const std::shared_ptr<LlmProvider>& provider) const {
    agent::RequestContextOptions options;
    options.cwd = cwd();
    options.skills = skill_registry_;
    options.memory = memory_registry_;
    if (memory_cfg_) options.memory_config = *memory_cfg_;
    if (project_instructions_cfg_) options.project_config = *project_instructions_cfg_;
    if (custom_instructions_cfg_) options.custom_config = *custom_instructions_cfg_;
    if (git_context_cfg_) options.git_config = *git_context_cfg_;
    if (expert_) options.expert = *expert_;
    options.expert_member = expert_member_id_;
    options.tool_policy = tool_capability_policy_;
    options.context_window = context_window_.load(std::memory_order_relaxed);
    const agent::ActiveModelView model(provider, options.context_window);
    options.model = model.prompt_state();
    options.can_read_images = model.can_read_images();
    options.loop_active = loop_execution_policy_.active;
    options.loop_context = loop_execution_policy_.system_context;
    options.swarm_mode = active_turn_swarm_mode_;
    options.environment = environment::prompt_environment();
    options.sandbox.description = sandbox_prompt_description();
    options.folders = system_prompt_workspace_folders();
    if (session_manager_) {
        const auto info = session_manager_->active_worktree();
        options.worktree = {info.active(), info.worktree_path, info.worktree_branch,
                            info.original_cwd, info.inherited};
    }
    options.skill_usage = skill_usage_store_;
    options.skill_idle_days = skill_idle_days_;
    return options;
}

std::vector<ChatMessage> AgentLoop::build_compaction_initial_context() const {
    const auto provider = provider_accessor_ ? provider_accessor_() : nullptr;
    return request_builder_->initial_context(request_context_options(provider));
}

AgentLoop::ApiRequestBundle AgentLoop::build_api_request_messages(
    const std::shared_ptr<LlmProvider>& provider, bool emergency_profile) {
    auto inputs = request_builder_->capture(request_context_options(provider),
                                            history_->view(), emergency_profile);
    if (!emergency_profile) {
        inputs.hook_context = drain_hook_request_context();
        if (permissions_.mode() == PermissionMode::Plan) {
            inputs.plan_context = agent::detail::build_plan_mode_context_prompt(
                session_manager_, tools_.is_allowed("AskUserQuestion", &tool_capability_policy_),
                tools_.is_allowed("ExitPlanMode", &tool_capability_policy_));
        }
        if (session_manager_) inputs.todos = session_manager_->current_todos();
    }
    return request_builder_->build(std::move(inputs));
}
void AgentLoop::invalidate_git_snapshot() {
    prompt_cache_->invalidate_git();
}
} // namespace acecode
