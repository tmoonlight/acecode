#include "tool_permission_gate.hpp"
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
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"
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

PermissionVerdict ToolPermissionGate::decide(
    const ToolCall& effective_tc, const ToolContext& tool_ctx,
    const std::string& ctx_path, const std::string& ctx_command,
    std::size_t tool_index, const ProgressEmitter& emit_progress) {
    ToolContext execution_context = tool_ctx;
    std::optional<sandbox::ExecPermission> exec_permission;
    if (auto denied = exec_.prepare(
            effective_tc, ctx_command, execution_context, exec_permission)) {
        return *denied;
    }


    // apply_patch 一份补丁涉及多条路径(Add / Update / Delete 与 Move
    // 目标),规则 / 写边界 / 危险路径逐条评估,任一路径不过整份补丁
    // 都不执行;其它工具沿用单个 ctx_path。空集合 = 该工具不带路径
    // (bash 等),规则匹配按空路径走一次以保持旧语义。
    std::vector<std::string> target_paths;
    if (effective_tc.function_name == "apply_patch") {
        target_paths = apply_patch::extract_target_paths(
            effective_tc.function_arguments, boundary_.cwd());
    } else if (!ctx_path.empty()) {
        target_paths.push_back(ctx_path);
    }
    const std::vector<std::string> rule_paths =
        target_paths.empty() ? std::vector<std::string>{std::string{}}
                             : target_paths;
    const bool is_file_mutation_tool =
        effective_tc.function_name == "file_write" ||
        effective_tc.function_name == "file_edit" ||
        effective_tc.function_name == "apply_patch";

    // 安全审计(openspec add-security-center D1):下面每个「决定已作出」
    // 的分支调一次 record_audit。bash 记命令原文,文件工具记首个路径
    // (多路径进 detail.paths),其它需确认的工具归 tool 类。
    PermissionAuditScope audit(
        security_, permissions_, session_manager_, effective_tc.function_name,
        ctx_command, ctx_path, target_paths, is_file_mutation_tool, exec_permission);

    if (is_file_mutation_tool) {
        for (const auto& target : target_paths) {
            auto path = path_from_utf8(target);
            if (path.is_relative()) path = path_from_utf8(boundary_.cwd()) / path;
            std::error_code ec;
            const auto normalized = std::filesystem::weakly_canonical(path, ec);
            const auto global_rules = std::filesystem::weakly_canonical(path_from_utf8(security_.environment().acecode_dir()) / "rules", ec);
            const auto relative = normalized.lexically_relative(global_rules);
            if (sandbox::is_exec_rules_path(target) || sandbox::is_exec_rules_path(path_to_utf8(normalized)) ||
                (!relative.empty() && *relative.begin() != "..")) {
                audit.record(security::kAuditDecisionForbidden, security::kAuditSourceRule,
                           "exec_rules_protected");
                return ToolResult{"[Permission denied] Exec rules must be edited by the user.", false};
            }
        }
    }
    // D10:只有内置保护规则(`.acecode/rules/**`,priority >= 1000)硬拒绝;
    // 配置里的普通 Deny(`.env` / `.git/**` 写入)交给 should_auto_allow 弹确认,
    // Desktop 没有 --dangerous 也有逃生口。yolo 的硬拒绝在下面单独处理;
    // bash 的配置 Deny 已在 evaluate_exec_permission 里映射成 forbidden。
    if (!permissions_.is_dangerous()) {
        for (const auto& rule_path : rule_paths) {
            const auto detail = permissions_.matched_rule_detail(
                effective_tc.function_name, rule_path, ctx_command);
            if (detail && detail->action == RuleAction::Deny &&
                detail->priority >= PermissionManager::kBuiltinProtectionPriority) {
                audit.record(security::kAuditDecisionForbidden, security::kAuditSourceRule,
                           "protected_rule");
                return ToolResult{"[Permission denied by configured rule]", false};
            }
        }
    }

    const bool targets_active_plan_file =
        permissions_.mode() == PermissionMode::Plan &&
        session_manager_ &&
        is_file_mutation_tool &&
        !target_paths.empty() &&
        std::all_of(target_paths.begin(), target_paths.end(),
                    [session = session_manager_](const std::string& target) {
                        return session->is_plan_file_path(target);
                    });
    bool auto_allow = true;
    for (const auto& rule_path : rule_paths) {
        if (!permissions_.should_auto_allow(
                effective_tc.function_name,
                tools_.is_read_only(effective_tc.function_name), rule_path, ctx_command)) {
            auto_allow = false;
        }
    }
    if (permissions_.mode() == PermissionMode::Plan) {
        auto_allow = tools_.is_read_only(effective_tc.function_name) ||
            targets_active_plan_file || effective_tc.function_name == "TodoWrite";
    }
    if (effective_tc.function_name == "ExitPlanMode" &&
        permissions_.mode() != PermissionMode::Plan) {
        auto_allow = true;
    }
    if (exec_permission) auto_allow = exec_permission->decision.verdict == sandbox::ExecVerdict::Allow;

    // In Yolo, should_auto_allow() can only be false when an
    // explicit Deny rule matched. Preserve that safety rule as a
    // hard rejection, but never turn it into a permission prompt.
    if (!auto_allow && permissions_.mode() == PermissionMode::Yolo) {
        audit.record(security::kAuditDecisionForbidden, security::kAuditSourceRule, "deny_rule_yolo");
        return ToolResult{
            "[Permission denied by configured rule in yolo mode]",
            false};
    }

    if (effective_tc.function_name == "bash" && command_looks_like_file_write(ctx_command)) {
        const std::string boundary_root = host_.write_root();
        if (!boundary_root.empty() &&
            permissions_.mode() == PermissionMode::Yolo &&
            !permissions_.is_dangerous()) {
            const std::string boundary_rejection =
                loop_shell_write_escape_reason(ctx_command, boundary_root,
                                               host_.writable_workspace_folders());
            if (!boundary_rejection.empty()) {
                audit.record(security::kAuditDecisionForbidden, security::kAuditSourceRule, "write_boundary");
                return ToolResult{"[Error] " + boundary_rejection, false};
            }
        }
        const auto failed_path = security_.safe_edit_guard().blocked_path(
            ctx_command, permissions_.is_dangerous() || permissions_.mode() == PermissionMode::Yolo);
        if (!failed_path.empty()) {
            audit.record(security::kAuditDecisionForbidden, security::kAuditSourceAuto, "safe_edit_guard");
            return ToolResult{
                "[Error] Shell write blocked for " + failed_path +
                " because a recent safe file edit failed. "
                "Re-read the file and retry with an exact " +
                model_tool_name_for_native("file_edit") +
                " old_string, or perform an explicit encoding conversion instead of bypassing text safety.",
                false};
        }
    }

    if (effective_tc.function_name != "bash") {
        for (const auto& target : target_paths) {
            std::string path_error =
                paths_.path_validation_error(effective_tc.function_name, target);
            if (!path_error.empty()) {
                LOG_WARN("Path validation failed: " + path_error);
                if (is_file_mutation_tool) {
                    audit.record(security::kAuditDecisionForbidden, security::kAuditSourceRule, "path_validation");
                }
                return ToolResult{"[Error] " + path_error, false};
            }
            if (!targets_active_plan_file &&
                boundary_.is_dangerous_path(target) && auto_allow &&
                !permissions_.is_dangerous() &&
                permissions_.mode() != PermissionMode::Yolo) {
                LOG_INFO("Dangerous path detected, forcing confirmation: " + target);
                auto_allow = false;
            }
        }
    }

    // Goal 无人值守模式:所有本会弹给用户的权限确认自动放行。
    // 放在 dangerous path 等 auto_allow 降级之后,保证 goal 运行
    // 期间绝不出现确认弹窗。Plan mode 在 goal_unattended_active
    // 内部被排除,只读约束不受影响。
    //
    // bash 也走这里:exec 决策为 Prompt 时按「用户已批准」执行,
    // 但 execution_context.exec_sandbox 仍是决策表给出的批准后
    // 沙盒(auto 下危险命令留在 workspace-write 里),不会因为
    // 无人值守就升级成完整访问;Forbidden 在上面已经返回,不受影响。
    // 曾经把 bash 排除在外:daemon 里 AsyncPrompter 会空等 5 分钟
    // 再 Deny,goal「绝不弹确认」的承诺被打破。
    if (auto_allow) {
        // 自动放行只记 bash 与写文件工具:只读工具每回合几十次,记了没人看。
        if (exec_permission) {
            const std::string& reason = exec_permission->decision.reason;
            const std::string source =
                reason == "session_allow" ? security::kAuditSourceSession
                : (reason == "rule_allow" || reason == "rule_allow_sandboxed") ? security::kAuditSourceRule
                                                                                : security::kAuditSourceAuto;
            audit.record(security::kAuditDecisionAllow, source, reason);
        } else if (is_file_mutation_tool) {
            const bool session = permissions_.has_session_allow(effective_tc.function_name);
            audit.record(security::kAuditDecisionAllow,
                       session ? security::kAuditSourceSession : security::kAuditSourceAuto,
                       session ? "session_allow"
                       : targets_active_plan_file ? "plan_file"
                       : std::string("mode_") + PermissionManager::mode_name(permissions_.mode()));
        }
    }
    if (!auto_allow && goal_.unattended_active(session_manager_)) {
        auto_allow = true;
        audit.record(security::kAuditDecisionAllow, security::kAuditSourceGoal, "unattended_goal");
        LOG_INFO("[goal] unattended auto-approve: " +
                 effective_tc.function_name +
                 (ctx_path.empty() ? std::string{} : " path=" + ctx_path) +
                 (exec_permission
                      ? " sandbox=" + std::string(sandbox::sandbox_mode_name(
                            exec_permission->decision.sandbox))
                      : std::string{}));
    }

    agent::PermissionHookSession permission_session(
        tool_hooks_, hook_manager_, session_manager_, effective_tc.function_name);

    if (!auto_allow && hook_manager_) {
        auto outcome = permission_session.request(
            exec_permission ? exec_permission->arguments :
            parse_tool_args_for_permission_payload(effective_tc.function_arguments));
        if (outcome.denied || outcome.blocked) {
            const std::string reason = outcome.reason.empty()
                ? "Permission denied by hook."
                : outcome.reason;
            permission_session.resolve("deny", "hook");
            audit.record(security::kAuditDecisionDeny, security::kAuditSourceHook, "hook_denied");
            return ToolResult{"[Hook denied permission] " + reason, false};
        }
        if (outcome.allowed) {
            auto_allow = true;
            permission_session.resolve("allow", "hook");
            audit.record(security::kAuditDecisionAllow, security::kAuditSourceHook, "hook_allowed");
        }
    }

    // Headless(-p / --print)模式:进程里没有任何交互通道能弹
    // 确认(无 TUI overlay / 无浏览器 WS)。走到这里 = 规则与
    // hook 都没放行,即将进交互 prompt —— AsyncPrompter 会空等
    // 5 分钟超时,必须短路。放在 hook 分支之后:hook 是非交互
    // 决策通道,headless 下依然应该先于兜底策略生效。
    //   - --yolo(dangerous):自动放行。
    //   - 其余(default/accept-edits/plan 的受限工具):直接拒绝,
    //     文案告知模型环境约束,引导改用只读方案而不是重试。
    if (!auto_allow && security_.environment().headless()) {
        if (permissions_.is_dangerous()) {
            auto_allow = true;
            permission_session.resolve("allow", "headless");
            audit.record(security::kAuditDecisionAllow, security::kAuditSourceHeadless, "headless_yolo");
            LOG_INFO("[headless] yolo auto-approve: " +
                     effective_tc.function_name +
                     (ctx_path.empty() ? std::string{} : " path=" + ctx_path));
        } else {
            LOG_INFO("[headless] denied (needs confirmation): " +
                     effective_tc.function_name);
            permission_session.resolve("deny", "headless");
            audit.record(security::kAuditDecisionDeny, security::kAuditSourceHeadless, "headless_no_channel");
            return ToolResult{
                "[Headless mode] This tool call requires interactive "
                "user confirmation, which is unavailable in print (-p) "
                "mode; it was denied automatically. Prefer a read-only "
                "alternative and continue. Rerun in an interactive "
                "session to approve this operation.",
                false};
        }
    }

    if (!auto_allow && confirmation_.available()) {
        if (auto denied = confirmation_.confirm(
                effective_tc, ctx_command, tool_index, emit_progress,
                exec_permission, execution_context, audit, permission_session)) {
            return *denied;
        }
        auto_allow = true;
    }

    if (exec_permission && !auto_allow) {
        permission_session.leave_unresolved();
        audit.record(security::kAuditDecisionDeny, security::kAuditSourceNone, "no_confirmation_channel");
        return ToolResult{"[Permission denied] This command requires approval, but no confirmation channel is available.", false};
    }

    // A non-interactive embedding may intentionally omit a
    // prompter while still allowing execution. Close the paired
    // lifecycle event before the tool starts in that case.
    if (!auto_allow && permission_session.pending()) {
        permission_session.resolve("allow", "implicit");
    }
    if (!auto_allow) {
        audit.record(security::kAuditDecisionAllow, security::kAuditSourceNone, "implicit");
    }

    return PermissionVerdict{
        std::move(execution_context), std::move(exec_permission), audit.sandbox()};

}

void ToolPermissionGate::observe_result(
    const PermissionVerdict& verdict, const ToolCall& call,
    const std::string& path, const std::string& command, ToolResult& result) {
    exec_.observe(verdict.exec_permission, verdict.audit_sandbox, command, result);
    security_.safe_edit_guard().record_result(call.function_name, path, result);
    if (call.function_name == "bash" && result.success &&
        command_looks_like_file_write(command)) {
        security_.safe_edit_guard().check_shell_output(command, result);
    }
}

} // namespace acecode::agent
