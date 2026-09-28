#include "permission_confirmation.hpp"
#include "permission_audit_scope.hpp"
#include "agent/tool_exec/tool_session_host.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "agent/callbacks_slot.hpp"
#include "utils/abort_signal.hpp"
#include "agent/hook_bridge/tool_hook_bridge.hpp"
#include "agent/approval/session_exec_security.hpp"
#include "agent/boundary/workspace_boundary.hpp"
#include "agent/approval/permission_payloads.hpp"
#include "agent/guards/doom_guard.hpp"
#include "agent/tool_exec/tool_batch_types.hpp"
#include "hooks/hook_manager.hpp"
#include "hooks/hook_runtime.hpp"
#include "llm/tool_protocol_names.hpp"
#include "permissions/interaction_mode.hpp"
#include "permissions/shell_write_guard.hpp"
#include "prompt/prompt_environment.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "sandbox/exec_permission.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/thread_goal_store.hpp"
#include "session/token_tracker.hpp"
#include "session/turn_timing.hpp"
#include "skills/skill_registry.hpp"
#include "tool/apply_patch_format.hpp"
#include "tool/text_file_errors.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"
#include "utils/text.hpp"
#include "utils/time.hpp"
#include "utils/uuid.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode::agent {

using agent::detail::parse_tool_args_for_permission_payload;
using agent::detail::build_plan_permission_args;
using utils::ascii_lower;

bool PermissionConfirmation::available() const {
    const auto callbacks = callbacks_.snapshot();
    return prompter_ || static_cast<bool>(callbacks.on_tool_confirm);
}

std::optional<ToolResult> PermissionConfirmation::confirm(
    const ToolCall& effective_tc, const std::string& ctx_command,
    std::size_t tool_index, const ProgressEmitter& emit_progress,
    std::optional<sandbox::ExecPermission>& exec_permission,
    ToolContext& execution_context, PermissionAuditScope& audit,
    PermissionHookSession& permission_session) {
    const auto callbacks = callbacks_.snapshot();

    emit_progress("permission_waiting", "正在等待权限确认",
        effective_tc.function_name, effective_tc.function_name, effective_tc.id,
        static_cast<int>(tool_index), true);
    const std::string permission_args =
        build_plan_permission_args(
            effective_tc.function_name,
            exec_permission ? exec_permission->arguments.dump() : effective_tc.function_arguments,
            session_manager_);
    PermissionResult perm = prompter_
        ? prompter_->prompt(effective_tc.function_name, permission_args, &abort_signal_.flag_for_legacy_api())
        : (callbacks.on_tool_confirm
            ? callbacks.on_tool_confirm(effective_tc.function_name, permission_args)
            : PermissionResult::Deny);
    if (perm == PermissionResult::Deny) {
        permission_session.resolve("deny", "interactive");
        audit.record(security::kAuditDecisionDeny, security::kAuditSourceUser,
                   exec_permission ? exec_permission->decision.reason : "confirmation");
        return ToolResult{"[User denied tool execution]", false};
    }
    // bash 专属决策落到别的工具(或 payload 没提供对应选项)时降级:
    // allow_scoped → allow,allow_remember → always_allow(D5 向后兼容)。
    const std::string scoped_root = exec_permission
        ? exec_permission->arguments["permission"].value("scoped_write_root", std::string{})
        : std::string{};
    if (perm == PermissionResult::AllowScoped && scoped_root.empty()) perm = PermissionResult::Allow;
    if (perm == PermissionResult::AllowRemember &&
        (!exec_permission || exec_permission->remember_patterns.empty())) {
        perm = PermissionResult::AlwaysAllow;
    }
    permission_session.resolve(
        perm == PermissionResult::AlwaysAllow   ? "always_allow"
        : perm == PermissionResult::AllowScoped   ? "allow_scoped"
        : perm == PermissionResult::AllowRemember ? "allow_remember"
                                                  : "allow",
        "interactive");
    audit.record(
        perm == PermissionResult::AlwaysAllow   ? security::kAuditDecisionAllowSession
        : perm == PermissionResult::AllowScoped   ? security::kAuditDecisionAllowScoped
        : perm == PermissionResult::AllowRemember ? security::kAuditDecisionAllowRemember
                                                  : security::kAuditDecisionAllow,
        security::kAuditSourceUser,
        exec_permission ? exec_permission->decision.reason : "confirmation");
    emit_progress("tool_running",
        "正在调用工具 " + effective_tc.function_name,
        effective_tc.function_name, effective_tc.function_name, effective_tc.id,
        static_cast<int>(tool_index), true);
    if (perm == PermissionResult::AllowScoped) {
        // D4:只放行建议目录 —— 记进会话授权,命令留在 workspace-write 里带着
        // 该目录执行,而不是整个出沙盒。
        sandbox::AdditionalPermissions grant;
        grant.write.push_back(scoped_root);
        security_.runtime().grant_for_session(grant);
        auto request = security_.runtime().request_for(sandbox::SandboxMode::WorkspaceWrite,
            host_.write_root().empty() ? host_.cwd() : host_.write_root());
        const auto error = security_.runtime().prepare_request(request);
        if (!error.empty()) {
            security_.runtime().mark_unavailable(error);
            return ToolResult{"[Sandbox unavailable] " + error +
                ". The scoped grant was recorded but the command was not executed; retry.", false};
        }
        execution_context.exec_sandbox = std::move(request);
        LOG_INFO("[sandbox] scoped grant for session: write " + scoped_root);
        security_.record_audit(session_manager_, security::kAuditCategoryRule, "bash", scoped_root,
                     security::kAuditDecisionAllowScoped, security::kAuditSourceUser,
                     "scoped_grant", sandbox::sandbox_mode_name(sandbox::SandboxMode::WorkspaceWrite),
                     nlohmann::json{{"command", ctx_command}});
    }
    if (perm == PermissionResult::AllowRemember && exec_permission) {
        // D6:写规则文件失败只记日志,本次仍按「允许一次」执行。
        const std::string remember_error = security_.remember_exec_rule(*exec_permission);
        security_.record_audit(session_manager_, security::kAuditCategoryRule, "bash", exec_permission->remember_display(),
                     security::kAuditDecisionAllowRemember, security::kAuditSourceUser,
                     remember_error.empty() ? "remember_rule" : "remember_rule_failed",
                     audit.sandbox(),
                     nlohmann::json{{"command", ctx_command}, {"error", remember_error}});
    }
    if ((perm == PermissionResult::AlwaysAllow || perm == PermissionResult::AllowRemember) &&
        permissions_.mode() != PermissionMode::Plan &&
        effective_tc.function_name != "EnterPlanMode" &&
        effective_tc.function_name != "ExitPlanMode") {
        if (exec_permission) {
            if (exec_permission->input.additional_requested && !exec_permission->additional.empty()) {
                // 额外权限申请的「本次会话允许」记的是权限,不是命令前缀。
                security_.runtime().grant_for_session(exec_permission->additional);
                nlohmann::json grant_detail{{"command", ctx_command}};
                grant_detail["read"] = exec_permission->additional.read;
                grant_detail["write"] = exec_permission->additional.write;
                grant_detail["network"] = exec_permission->additional.network;
                security_.record_audit(session_manager_, security::kAuditCategoryRule, "bash",
                             exec_permission->additional.write.empty()
                                 ? (exec_permission->additional.read.empty() ? std::string("network")
                                                                              : exec_permission->additional.read.front())
                                 : exec_permission->additional.write.front(),
                             security::kAuditDecisionAllowSession, security::kAuditSourceUser,
                             "session_grant", audit.sandbox(), std::move(grant_detail));
            } else {
                for (const auto& prefix : exec_permission->prefixes) {
                    permissions_.add_session_command_allow(prefix,
                        exec_permission->decision.sandbox == sandbox::SandboxMode::FullAccess &&
                        exec_permission->input.escalation_requested);
                }
            }
        } else {
            permissions_.add_session_allow(effective_tc.function_name);
        }
    }


    return std::nullopt;
}
} // namespace acecode::agent
