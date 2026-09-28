#include "exec_permission_gate.hpp"
#include "agent/tool_exec/tool_session_host.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "agent/agent_callbacks.hpp"
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

std::optional<ToolResult> ExecPermissionGate::prepare(
    const ToolCall& effective_tc, const std::string& ctx_command,
    ToolContext& execution_context, std::optional<sandbox::ExecPermission>& exec_permission) {
    if (effective_tc.function_name == "bash") {
        auto platform = sandbox::host_command_platform();
        const auto environment = acecode::environment::prompt_environment();
        if (environment.terminal_family == "powershell") platform = sandbox::CommandPlatform::PowerShell;
        else if (environment.terminal_family == "bash" || environment.terminal_family == "posix") {
            platform = sandbox::CommandPlatform::Posix;
        }
        sandbox::ExecPermissionOptions exec_options;
        exec_options.unattended = goal_.unattended_active(session_manager_);
        exec_options.session_grants = security_.runtime().session_grants();
        exec_permission = security_.evaluate_exec(
            effective_tc.function_arguments, platform, exec_options);
        if (!exec_permission->error.empty()) return ToolResult{"[Error] " + exec_permission->error, false};
        if (exec_permission->decision.verdict == sandbox::ExecVerdict::Forbidden) {
            const bool unattended_forbidden =
                exec_permission->decision.reason == "escalation_unattended";
            security_.record_audit(session_manager_, security::kAuditCategoryCommand, "bash", ctx_command,
                security::kAuditDecisionForbidden,
                unattended_forbidden ? security::kAuditSourceGoal : security::kAuditSourceRule,
                exec_permission->decision.reason,
                sandbox::sandbox_mode_name(exec_permission->decision.sandbox),
                nlohmann::json{{"mode", PermissionManager::mode_name(permissions_.mode())},
                               {"command_kind", sandbox::command_kind_name(exec_permission->classification.kind)}});
            if (unattended_forbidden) {
                // D1:无人值守没有人能批越权;不是拒绝命令本身,只是拒绝加宽。
                return ToolResult{
                    "[Sandbox] Escalated or additional permissions cannot be approved while running "
                    "unattended (active goal), so this call was not executed. Retry the same command "
                    "without sandbox_permissions / with_escalated_permissions / additional_permissions; "
                    "it will run inside the sandbox.", false};
            }
            return ToolResult{"[Permission denied by configured exec rule]", false};
        }
        const std::string sandbox_root = host_.write_root().empty() ? boundary_.cwd() : host_.write_root();
        // D4:越权确认里附上上一次被拒的路径,并在它不在 deny 名单、且能用
        // workspace-write 承载时提供「只放行该目录」选项。
        const auto last_violation = security_.feedback();
        if (exec_permission->decision.verdict == sandbox::ExecVerdict::Prompt &&
            exec_permission->input.escalation_requested && last_violation &&
            permissions_.mode() != PermissionMode::Plan) {
            if (!last_violation->path.empty()) {
                exec_permission->arguments["permission"]["denied_path"] = last_violation->path;
            }
            if (!security_.session_disabled() && security_.runtime().available()) {
                const auto baseline = security_.runtime().policy_for(sandbox::SandboxMode::WorkspaceWrite, sandbox_root);
                const auto suggested = sandbox::suggested_write_root(*last_violation, baseline);
                if (!suggested.empty()) {
                    exec_permission->arguments["permission"]["scoped_write_root"] = suggested;
                }
            }
        }
        if (exec_permission->decision.sandbox != sandbox::SandboxMode::FullAccess) {
            const sandbox::AdditionalPermissions* extra =
                exec_permission->additional.empty() ? nullptr : &exec_permission->additional;
            auto request = security_.runtime().request_for(exec_permission->decision.sandbox,
                                                        sandbox_root, extra);
            const auto error = security_.runtime().prepare_request(request);
            if (!error.empty()) {
                security_.runtime().mark_unavailable(error);
                exec_permission->set_availability(false);
            } else {
                execution_context.exec_sandbox = std::move(request);
            }
        }
    }
    return std::nullopt;
}

void ExecPermissionGate::observe(
    const std::optional<sandbox::ExecPermission>& exec_permission,
    const std::string& audit_sandbox, const std::string& ctx_command,
    ToolResult& tool_result) {
    if (exec_permission && tool_result.metadata.value("sandbox_unavailable", false)) {
        // 只记一行原因:它会进 /sandbox 状态与 system prompt 的
        // `Shell sandbox:` 行,整段工具输出塞进去既难读也浪费上下文。
        std::string reason = tool_result.metadata.value(
            "sandbox_unavailable_reason", std::string{});
        if (reason.empty()) {
            reason = tool_result.output.substr(0, tool_result.output.find('\n'));
        }
        security_.runtime().mark_unavailable(reason);
    }
    if (exec_permission) {
        // D4:记住最近一次沙盒拒绝(含路径),给下一次越权确认提供
        // 「只放行该目录」;bash 成功一次就作废,别拿陈旧路径误导用户。
        const auto& meta = tool_result.metadata;
        if (meta.contains("sandbox_violation") && meta["sandbox_violation"].is_object()) {
            sandbox::SandboxViolation violation;
            violation.reason = meta["sandbox_violation"].value("reason", std::string{});
            violation.path = meta["sandbox_violation"].value("path", std::string{});
            violation.snippet = meta["sandbox_violation"].value("snippet", std::string{});
            // 被拒路径单独入账(category=sandbox):文件安全页的「最近被拦路径」
            // 按 target 聚合,所以 target 只放路径,抽不到就留空、命令进 detail。
            security_.record_audit(session_manager_, security::kAuditCategorySandbox, "bash", violation.path,
                         security::kAuditDecisionBlocked, security::kAuditSourceSandbox,
                         violation.reason, audit_sandbox,
                         nlohmann::json{{"command", ctx_command}, {"snippet", violation.snippet}});
            security_.set_feedback(std::move(violation));
        } else if (tool_result.success) {
            security_.set_feedback(std::nullopt);
        }
    }

}
} // namespace acecode::agent
