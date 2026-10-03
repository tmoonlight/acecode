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
#include "memory/memory_service.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"

namespace acecode::agent {
RequestContextOptions RequestContextFactory::options(
    const std::shared_ptr<LlmProvider>& provider) const {
    agent::RequestContextOptions options;
    options.cwd = boundary_.cwd();
    options.skills = source_.skills;
    options.memory = source_.memory.get();
    // 记忆开关以记忆服务的运行时配置为准(设置页改了立即对新请求生效)。
    options.memory_config = source_.memory ? std::optional<MemoryConfig>(source_.memory->config())
                                           : source_.prompt_config.memory;
    if (session_manager_) {
        options.memory_session_key = session_manager_->current_session_id();
        if (!session_manager_->is_no_workspace()) {
            options.memory_project_dir = session_manager_->current_project_dir();
        }
        if (!session_manager_->memory_enabled() && options.memory_config) {
            options.memory_config->enabled = false;
        }
    } else if (!options.cwd.empty()) {
        options.memory_project_dir = SessionStorage::get_project_dir(options.cwd);
    }
    options.project_config = source_.prompt_config.project_instructions;
    options.custom_config = source_.prompt_config.custom_instructions;
    options.git_config = source_.prompt_config.git_context;
    if (source_.expert) options.expert = *source_.expert;
    options.expert_member = source_.expert_member;
    options.tool_policy = source_.tool_policy;
    options.context_window = context_window_.load(std::memory_order_relaxed);
    const agent::ActiveModelView model(provider, options.context_window, source_.runtime);
    options.model = model.prompt_state();
    options.can_read_images = model.can_read_images();
    options.loop_active = source_.loop.active;
    options.loop_context = source_.loop.system_context;
    options.swarm = source_.swarm;
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
    const std::shared_ptr<LlmProvider>& provider, bool emergency_profile) {
    auto inputs = builder_.capture(options(provider),
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
