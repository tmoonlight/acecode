#include "request_context_factory.hpp"
#include "request_context_source.hpp"
#include "request_context.hpp"
#include "agent/approval/session_exec_security.hpp"
#include "agent/boundary/workspace_boundary.hpp"
#include "agent/hook_bridge/agent_hook_bridge.hpp"
#include "agent/model_step/active_model_view.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "permissions/permissions.hpp"
#include "prompt/prompt_environment.hpp"
#include "session/session_manager.hpp"

namespace acecode::agent {
RequestContextOptions RequestContextFactory::options(
    const std::shared_ptr<LlmProvider>& provider, bool swarm_mode) const {
    agent::RequestContextOptions options;
    options.cwd = boundary_.cwd();
    options.skills = source_.skills;
    options.memory = source_.memory;
    if (source_.memory_config) options.memory_config = *source_.memory_config;
    if (source_.project_config) options.project_config = *source_.project_config;
    if (source_.custom_config) options.custom_config = *source_.custom_config;
    if (source_.git_config) options.git_config = *source_.git_config;
    if (source_.expert) options.expert = *source_.expert;
    options.expert_member = source_.expert_member;
    options.tool_policy = source_.tool_policy;
    options.context_window = context_window_.load(std::memory_order_relaxed);
    const agent::ActiveModelView model(provider, options.context_window, source_.runtime);
    options.model = model.prompt_state();
    options.can_read_images = model.can_read_images();
    options.loop_active = source_.loop.active;
    options.loop_context = source_.loop.system_context;
    options.swarm_mode = swarm_mode;
    options.environment = source_.runtime.prompt_environment();
    options.sandbox.description = security_.sandbox_prompt_description(session_manager_);
    options.folders = boundary_.system_prompt_workspace_folders(session_manager_);
    if (session_manager_) {
        const auto info = session_manager_->active_worktree();
        options.worktree = {info.active(), info.worktree_path, info.worktree_branch,
                            info.original_cwd, info.inherited};
    }
    options.skill_usage = source_.skill_usage;
    options.skill_idle_days = source_.skill_idle_days;
    return options;
}

ApiRequestBundle RequestContextFactory::build(
    const std::shared_ptr<LlmProvider>& provider, bool emergency_profile, bool swarm_mode) {
    auto inputs = builder_.capture(options(provider, swarm_mode),
                                            history_.view(), emergency_profile);
    if (!emergency_profile) {
        inputs.hook_context = hooks_.drain_context();
        if (permissions_.mode() == PermissionMode::Plan) {
            inputs.plan_context = agent::detail::build_plan_mode_context_prompt(
                session_manager_, tools_.is_allowed("AskUserQuestion", &source_.tool_policy),
                tools_.is_allowed("ExitPlanMode", &source_.tool_policy), source_.runtime.mtime_tracker());
        }
        if (session_manager_) inputs.todos = session_manager_->current_todos();
    }
    return builder_.build(std::move(inputs));
}
} // namespace acecode::agent
