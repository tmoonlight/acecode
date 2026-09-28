#include "tool_context_factory.hpp"
#include "agent/approval/session_exec_security.hpp"
#include "agent/boundary/workspace_boundary.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "agent/request/prompt_context_cache.hpp"
#include "agent/detail/agent_payloads.hpp"
#include "config/config.hpp"
#include "permissions/permissions.hpp"
#include "session/event_dispatcher.hpp"
#include "session/session_manager.hpp"
#include "session/thread_goal_store.hpp"
#include "utils/abort_signal.hpp"

namespace acecode::agent {
using detail::build_session_scratch_dir;

ToolContextFactory::ToolContextFactory(
    WorkspaceBoundary& boundary, SessionExecSecurity& security, PromptContextCache& cache,
    PermissionManager& permissions, GoalRuntime& goal, EventDispatcher& events,
    AbortSignal& abort, SessionManager* session, const SkillRegistry* skills,
    const ToolCapabilityPolicy& policy, const AgentLoopConfig& config,
    ProviderAccessor provider)
    : boundary_(boundary), security_(security), cache_(cache), permissions_(permissions),
      goal_(goal), events_(events), abort_signal_(abort), session_manager_(session),
      skill_registry_(skills), capability_policy_(policy), loop_cfg_(config),
      provider_accessor_(std::move(provider)) {}

std::string ToolContextFactory::cwd() const { return boundary_.cwd(); }
std::string ToolContextFactory::write_root() const { return boundary_.write_root(session_manager_); }
std::vector<std::string> ToolContextFactory::writable_workspace_folders() const {
    return boundary_.writable_workspace_folders(session_manager_);
}
bool ToolContextFactory::path_in_workspace_folders(const std::string& path) const {
    return boundary_.path_in_workspace_folders(path, session_manager_);
}
void ToolContextFactory::switch_cwd(const std::string& cwd) {
    switch_cwd(boundary_, security_, cache_, permissions_, session_manager_, cwd);
}
void ToolContextFactory::switch_cwd(
    WorkspaceBoundary& boundary, SessionExecSecurity& security, PromptContextCache& cache,
    PermissionManager& permissions, SessionManager* session, const std::string& cwd) {
    boundary.set_cwd(cwd);
    cache.reset_on_cwd_change();
    permissions.clear_session_allows();
    security.runtime().clear_session_grants();
    security.set_feedback(std::nullopt);
    security.reload_exec_rules();
    security.runtime().set_workspace_writable_roots(boundary.writable_workspace_folders(session));
}
ToolContext ToolContextFactory::for_user_shell(
    WorkspaceBoundary& boundary, AbortSignal& abort, SessionManager* session) {
    ToolContext context;
    context.cwd = boundary.cwd();
    context.write_root = boundary.write_root(session);
    context.abort_flag = &abort.flag_for_legacy_api();
    context.session_manager = session;
    context.scratch_dir = build_session_scratch_dir(boundary.cwd(), session);
    return context;
}

ToolContext ToolContextFactory::for_tool() {
    ToolContext tool_ctx;
    tool_ctx.cwd = boundary_.cwd();
    tool_ctx.write_root = write_root();
    tool_ctx.abort_flag = &abort_signal_.flag_for_legacy_api();
    tool_ctx.session_manager = session_manager_;
    if (session_manager_) {
        tool_ctx.session_id = session_manager_->current_session_id();
        tool_ctx.parent_session_id =
            session_manager_->current_parent_session_id();
        // 工作区 hash = projects/<hash> 目录名。手工切最后一段,不经
        // std::filesystem::path:UTF-8 路径按系统代码页隐式转换会在中文目录下
        // 抛异常(见 CLAUDE.md「cwd 一律以 UTF-8 std::string 传递」)。
        const std::string project_dir = session_manager_->current_project_dir();
        const std::size_t cut = project_dir.find_last_of("/\\");
        tool_ctx.workspace_hash = cut == std::string::npos
            ? project_dir : project_dir.substr(cut + 1);
    }
    tool_ctx.skill_registry = skill_registry_;
    tool_ctx.scratch_dir = build_session_scratch_dir(boundary_.cwd(), session_manager_);
    tool_ctx.preserve_full_output = true;
    tool_ctx.capability_policy = capability_policy_;
    // 模型身份在回合内固定(切换只发生在回合边界),所以在这里取一次快照即可。
    if (provider_accessor_) {
        if (const std::shared_ptr<LlmProvider> provider = provider_accessor_()) {
            tool_ctx.active_provider_name = provider->name();
            tool_ctx.active_model_id = provider->model();
            tool_ctx.active_model_can_read_images = provider->supports_vision();
        }
    }
    const auto ref = lifetime_.ref(*this);
    tool_ctx.account_goal_usage = [ref]() {
        ref.with([&](ToolContextFactory& host) { host.account_goal_usage(); });
    };
    tool_ctx.emit_goal_updated = [ref](const nlohmann::json& goal_payload) {
        ref.with([&](ToolContextFactory& host) { host.emit_goal_updated(goal_payload); });
    };
    tool_ctx.emit_goal_cleared = [ref](const std::string& session_id) {
        ref.with([&](ToolContextFactory& host) { host.emit_goal_cleared(session_id); });
    };
    tool_ctx.emit_todo_updated = [ref](const nlohmann::json& todo_payload) {
        ref.with([&](ToolContextFactory& host) { host.emit_todo_updated(todo_payload); });
    };
    tool_ctx.goal_unattended_active = [ref]() {
        bool result = false;
        ref.with([&](ToolContextFactory& host) { result = host.goal_unattended_active(); });
        return result;
    };
    tool_ctx.current_permission_mode = [ref]() {
        std::string result{};
        ref.with([&](ToolContextFactory& host) { result = host.current_permission_mode(); });
        return result;
    };
    tool_ctx.question_policy = [ref]() {
        ResolvedQuestionPolicy result{};
        ref.with([&](ToolContextFactory& host) { result = host.question_policy(); });
        return result;
    };
    tool_ctx.enter_plan_mode = [ref]() {
        std::string result{};
        ref.with([&](ToolContextFactory& host) { result = host.enter_plan_mode(); });
        return result;
    };
    tool_ctx.exit_plan_mode = [ref]() {
        std::string result{};
        ref.with([&](ToolContextFactory& host) { result = host.exit_plan_mode(); });
        return result;
    };
    tool_ctx.switch_session_cwd = [ref](const std::string& new_cwd) {
        ref.with([&](ToolContextFactory& host) { host.switch_session_cwd(new_cwd); });
    };
    if (session_manager_) {
        tool_ctx.track_file_write_before = [ref](const std::string& path) {
            ref.with([&](ToolContextFactory& host) { host.track_file_write_before(path); });
        };
    }
    return tool_ctx;
}

void ToolContextFactory::account_goal_usage() {
    goal_.account_usage(session_manager_, 0, true);
}

void ToolContextFactory::emit_goal_updated(const nlohmann::json& goal_payload) {
    if (session_manager_) {
        const std::string sid = session_manager_->current_session_id();
        ThreadGoalStore* store = session_manager_->existing_goal_store();
        if (store && !sid.empty()) {
            auto goal = store->get_thread_goal(sid);
            if (goal.has_value()) {
                goal_.emit_updated(*goal);
                return;
            }
        }
        events_.emit(SessionEventKind::GoalUpdated,
            nlohmann::json{{"session_id", sid}, {"goal", goal_payload}});
    }
}

void ToolContextFactory::emit_goal_cleared(const std::string& session_id) {
    goal_.emit_cleared(session_id);
}

void ToolContextFactory::emit_todo_updated(const nlohmann::json& todo_payload) {
    goal_.emit_todo_updated(session_manager_, todo_payload);
}

bool ToolContextFactory::goal_unattended_active() {
    return goal_.unattended_active(session_manager_);
}

std::string ToolContextFactory::current_permission_mode() {
    const PermissionMode mode = permissions_.mode();
    // An explicitly selected Plan mode is authoritative even when the
    // process was started with --yolo/--dangerous. Otherwise the plan
    // prompt remains active while ExitPlanMode sees "yolo" and no-ops.
    if (mode == PermissionMode::Plan) {
        return std::string{"plan"};
    }
    if (permissions_.is_dangerous() || mode == PermissionMode::Yolo) {
        return std::string{"yolo"};
    }
    return std::string(PermissionManager::mode_name(mode));
}

ResolvedQuestionPolicy ToolContextFactory::question_policy() {
    return resolve_question_policy(loop_cfg_);
}

std::string ToolContextFactory::enter_plan_mode() {
    if (permissions_.is_dangerous() ||
        permissions_.mode() == PermissionMode::Yolo) {
        return std::string{};
    }
    permissions_.set_mode(PermissionMode::Plan);
    permissions_.clear_session_allows();
    std::string plan_file;
    if (session_manager_) {
        session_manager_->set_permission_mode("plan");
        session_manager_->set_pre_plan_permission_mode(
            PermissionManager::mode_name(permissions_.pre_plan_mode()));
        plan_file = session_manager_->ensure_plan_file_path();
    }
    return plan_file;
}

std::string ToolContextFactory::exit_plan_mode() {
    PermissionMode restored = permissions_.restore_pre_plan_mode();
    const std::string restored_name = PermissionManager::mode_name(restored);
    if (session_manager_) {
        session_manager_->set_permission_mode(restored_name);
        session_manager_->set_pre_plan_permission_mode(std::string{});
    }
    return restored_name;
}

void ToolContextFactory::switch_session_cwd(const std::string& new_cwd) {
    switch_cwd(new_cwd);
}

void ToolContextFactory::track_file_write_before(const std::string& path) {
    if (session_manager_) session_manager_->track_file_write_before(path);
}

} // namespace acecode::agent
