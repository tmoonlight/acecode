#include "agent/agent_loop.hpp"
#include "agent/boundary/workspace_boundary.hpp"
#include "agent/detail/agent_payloads.hpp"
#include "agent/tool_exec/tool_batch_types.hpp"
#include "permissions/interaction_mode.hpp"
#include "permissions/shell_write_guard.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/thread_goal_store.hpp"
#include "session/turn_timing.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/text.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode {

using agent::detail::build_session_scratch_dir;

ToolContext AgentLoop::build_tool_context() {
    ToolContext tool_ctx;
    tool_ctx.cwd = boundary_->cwd();
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
    tool_ctx.scratch_dir = build_session_scratch_dir(boundary_->cwd(), session_manager_);
    tool_ctx.preserve_full_output = true;
    tool_ctx.capability_policy = tool_capability_policy_;
    // 模型身份在回合内固定(切换只发生在回合边界),所以在这里取一次快照即可。
    if (provider_accessor_) {
        if (const std::shared_ptr<LlmProvider> provider = provider_accessor_()) {
            tool_ctx.active_provider_name = provider->name();
            tool_ctx.active_model_id = provider->model();
            tool_ctx.active_model_can_read_images = provider->supports_vision();
        }
    }
    tool_ctx.account_goal_usage = [this]() {
        account_goal_usage(0, true);
    };
    tool_ctx.emit_goal_updated = [this](const nlohmann::json& goal_payload) {
        if (session_manager_) {
            const std::string sid = session_manager_->current_session_id();
            ThreadGoalStore* store = session_manager_->existing_goal_store();
            if (store && !sid.empty()) {
                auto goal = store->get_thread_goal(sid);
                if (goal.has_value()) {
                    emit_goal_updated(*goal);
                    return;
                }
            }
            events_.emit(SessionEventKind::GoalUpdated,
                nlohmann::json{{"session_id", sid}, {"goal", goal_payload}});
        }
    };
    tool_ctx.emit_goal_cleared = [this](const std::string& session_id) {
        emit_goal_cleared(session_id);
    };
    tool_ctx.emit_todo_updated = [this](const nlohmann::json& todo_payload) {
        emit_todo_updated(todo_payload);
    };
    tool_ctx.goal_unattended_active = [this]() {
        return goal_unattended_active();
    };
    tool_ctx.current_permission_mode = [this]() {
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
    };
    tool_ctx.question_policy = [this]() {
        return resolved_question_policy();
    };
    tool_ctx.enter_plan_mode = [this]() {
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
    };
    tool_ctx.exit_plan_mode = [this]() {
        PermissionMode restored = permissions_.restore_pre_plan_mode();
        const std::string restored_name = PermissionManager::mode_name(restored);
        if (session_manager_) {
            session_manager_->set_permission_mode(restored_name);
            session_manager_->set_pre_plan_permission_mode(std::string{});
        }
        return restored_name;
    };
    tool_ctx.switch_session_cwd = [this](const std::string& new_cwd) {
        set_cwd(new_cwd);
    };
    if (session_manager_) {
        tool_ctx.track_file_write_before = [this](const std::string& path) {
            if (session_manager_) {
                session_manager_->track_file_write_before(path);
            }
        };
    }
    return tool_ctx;
}

} // namespace acecode
