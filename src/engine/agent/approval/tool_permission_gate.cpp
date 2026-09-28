#include "agent/agent_loop.hpp"
#include "agent/approval/permission_payloads.hpp"
#include "agent/guards/doom_guard.hpp"
#include "agent/tool_exec/tool_batch_types.hpp"
#include "hooks/hook_manager.hpp"
#include "hooks/hook_runtime.hpp"
#include "llm/tool_protocol_names.hpp"
#include "pa/pa_context_budget.hpp"
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

namespace acecode {

using agent::detail::parse_tool_args_for_permission_payload;
using agent::detail::build_plan_permission_args;
using utils::ascii_lower;

ToolResult AgentLoop::run_write_tool(ToolBatchState& batch, const ToolCall& effective_tc, const ToolContext& tool_ctx, const std::string& ctx_path, const std::string& ctx_command, size_t tool_index) {
    const auto& emit_progress = batch.emit_progress;
    const ToolCapabilityPolicy* policy =
        tool_ctx.capability_policy
            ? &*tool_ctx.capability_policy
            : nullptr;
    if (tools_.is_denied_by_policy(
            effective_tc.function_name, policy)) {
        return ToolResult{
            "[Error] Tool denied by the active expert capability "
            "policy: " + effective_tc.function_name,
            false};
    }
    if (auto guarded = maybe_guard_tool(batch, effective_tc)) {
        return *guarded;
    }

    ToolContext execution_context = tool_ctx;
    std::optional<sandbox::ExecPermission> exec_permission;
    if (effective_tc.function_name == "bash") {
        auto platform = sandbox::host_command_platform();
        const auto environment = acecode::environment::prompt_environment();
        if (environment.terminal_family == "powershell") platform = sandbox::CommandPlatform::PowerShell;
        else if (environment.terminal_family == "bash" || environment.terminal_family == "posix") {
            platform = sandbox::CommandPlatform::Posix;
        }
        sandbox::ExecPermissionOptions exec_options;
        exec_options.unattended = goal_unattended_active();
        exec_options.session_grants = sandbox_runtime_.session_grants();
        exec_permission = sandbox::evaluate_exec_permission(effective_tc.function_arguments,
            permissions_, exec_rules_, !sandbox_session_disabled_ && sandbox_runtime_.available(),
            platform, exec_options);
        if (!exec_permission->error.empty()) return ToolResult{"[Error] " + exec_permission->error, false};
        if (exec_permission->decision.verdict == sandbox::ExecVerdict::Forbidden) {
            const bool unattended_forbidden =
                exec_permission->decision.reason == "escalation_unattended";
            record_audit(security::kAuditCategoryCommand, "bash", ctx_command,
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
        const std::string sandbox_root = write_root().empty() ? cwd_ : write_root();
        // D4:越权确认里附上上一次被拒的路径,并在它不在 deny 名单、且能用
        // workspace-write 承载时提供「只放行该目录」选项。
        if (exec_permission->decision.verdict == sandbox::ExecVerdict::Prompt &&
            exec_permission->input.escalation_requested && last_sandbox_violation_ &&
            permissions_.mode() != PermissionMode::Plan) {
            if (!last_sandbox_violation_->path.empty()) {
                exec_permission->arguments["permission"]["denied_path"] = last_sandbox_violation_->path;
            }
            if (!sandbox_session_disabled_ && sandbox_runtime_.available()) {
                const auto baseline = sandbox_runtime_.policy_for(sandbox::SandboxMode::WorkspaceWrite, sandbox_root);
                const auto suggested = sandbox::suggested_write_root(*last_sandbox_violation_, baseline);
                if (!suggested.empty()) {
                    exec_permission->arguments["permission"]["scoped_write_root"] = suggested;
                }
            }
        }
        if (exec_permission->decision.sandbox != sandbox::SandboxMode::FullAccess) {
            const sandbox::AdditionalPermissions* extra =
                exec_permission->additional.empty() ? nullptr : &exec_permission->additional;
            auto request = sandbox_runtime_.request_for(exec_permission->decision.sandbox,
                                                        sandbox_root, extra);
            const auto error = sandbox_runtime_.prepare_request(request);
            if (!error.empty()) {
                sandbox_runtime_.mark_unavailable(error);
                exec_permission->set_availability(false);
            } else {
                execution_context.exec_sandbox = std::move(request);
            }
        }
    }

    // apply_patch 一份补丁涉及多条路径(Add / Update / Delete 与 Move
    // 目标),规则 / 写边界 / 危险路径逐条评估,任一路径不过整份补丁
    // 都不执行;其它工具沿用单个 ctx_path。空集合 = 该工具不带路径
    // (bash 等),规则匹配按空路径走一次以保持旧语义。
    std::vector<std::string> target_paths;
    if (effective_tc.function_name == "apply_patch") {
        target_paths = apply_patch::extract_target_paths(
            effective_tc.function_arguments, cwd_);
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
    const std::string audit_category =
        effective_tc.function_name == "bash" ? security::kAuditCategoryCommand
        : is_file_mutation_tool ? security::kAuditCategoryFile
                                : security::kAuditCategoryTool;
    const std::string audit_target = effective_tc.function_name == "bash"
        ? ctx_command
        : (!target_paths.empty() ? target_paths.front() : ctx_path);
    const std::string audit_sandbox = exec_permission
        ? std::string(sandbox::sandbox_mode_name(exec_permission->decision.sandbox))
        : std::string{};
    const auto audit_detail = [&]() {
        nlohmann::json detail = nlohmann::json::object();
        detail["mode"] = PermissionManager::mode_name(permissions_.mode());
        if (target_paths.size() > 1) detail["paths"] = target_paths;
        if (exec_permission) {
            detail["command_kind"] = sandbox::command_kind_name(exec_permission->classification.kind);
            detail["decision_reason"] = exec_permission->decision.reason;
            if (exec_permission->input.escalation_requested) detail["escalation_requested"] = true;
            if (exec_permission->input.additional_requested) detail["additional_requested"] = true;
        }
        return detail;
    };
    const auto audit_gate = [&](const std::string& decision, const std::string& source,
                                const std::string& reason) {
        record_audit(audit_category, effective_tc.function_name, audit_target,
                     decision, source, reason, audit_sandbox, audit_detail());
    };

    if (is_file_mutation_tool) {
        for (const auto& target : target_paths) {
            auto path = path_from_utf8(target);
            if (path.is_relative()) path = path_from_utf8(cwd_) / path;
            std::error_code ec;
            const auto normalized = std::filesystem::weakly_canonical(path, ec);
            const auto global_rules = std::filesystem::weakly_canonical(path_from_utf8(get_acecode_dir()) / "rules", ec);
            const auto relative = normalized.lexically_relative(global_rules);
            if (sandbox::is_exec_rules_path(target) || sandbox::is_exec_rules_path(path_to_utf8(normalized)) ||
                (!relative.empty() && *relative.begin() != "..")) {
                audit_gate(security::kAuditDecisionForbidden, security::kAuditSourceRule,
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
                audit_gate(security::kAuditDecisionForbidden, security::kAuditSourceRule,
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
                    [this](const std::string& target) {
                        return session_manager_->is_plan_file_path(target);
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
        audit_gate(security::kAuditDecisionForbidden, security::kAuditSourceRule, "deny_rule_yolo");
        return ToolResult{
            "[Permission denied by configured rule in yolo mode]",
            false};
    }

    if (effective_tc.function_name == "bash" && command_looks_like_file_write(ctx_command)) {
        const std::string boundary_root = write_root();
        if (!boundary_root.empty() &&
            permissions_.mode() == PermissionMode::Yolo &&
            !permissions_.is_dangerous()) {
            const std::string boundary_rejection =
                loop_shell_write_escape_reason(ctx_command, boundary_root,
                                               writable_workspace_folders());
            if (!boundary_rejection.empty()) {
                audit_gate(security::kAuditDecisionForbidden, security::kAuditSourceRule, "write_boundary");
                return ToolResult{"[Error] " + boundary_rejection, false};
            }
        }
        const auto now = std::chrono::steady_clock::now();
        for (auto it = recent_safe_edit_failures_.begin();
             it != recent_safe_edit_failures_.end();) {
            if (now - it->second > std::chrono::minutes(10)) {
                it = recent_safe_edit_failures_.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& [failed_path, when] : recent_safe_edit_failures_) {
            (void)when;
            if (command_mentions_path(ctx_command, failed_path) &&
                !permissions_.is_dangerous() &&
                permissions_.mode() != PermissionMode::Yolo) {
                audit_gate(security::kAuditDecisionForbidden, security::kAuditSourceAuto, "safe_edit_guard");
                return ToolResult{
                    "[Error] Shell write blocked for " + failed_path +
                    " because a recent safe file edit failed. "
                    "Re-read the file and retry with an exact " +
                    model_tool_name_for_native("file_edit") +
                    " old_string, or perform an explicit encoding conversion instead of bypassing text safety.",
                    false};
            }
        }
    }

    if (effective_tc.function_name != "bash") {
        for (const auto& target : target_paths) {
            std::string path_error =
                path_validation_error(effective_tc.function_name, target);
            if (!path_error.empty()) {
                LOG_WARN("Path validation failed: " + path_error);
                if (is_file_mutation_tool) {
                    audit_gate(security::kAuditDecisionForbidden, security::kAuditSourceRule, "path_validation");
                }
                return ToolResult{"[Error] " + path_error, false};
            }
            if (!targets_active_plan_file &&
                path_validator_.is_dangerous_path(target) && auto_allow &&
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
            audit_gate(security::kAuditDecisionAllow, source, reason);
        } else if (is_file_mutation_tool) {
            const bool session = permissions_.has_session_allow(effective_tc.function_name);
            audit_gate(security::kAuditDecisionAllow,
                       session ? security::kAuditSourceSession : security::kAuditSourceAuto,
                       session ? "session_allow"
                       : targets_active_plan_file ? "plan_file"
                       : std::string("mode_") + PermissionManager::mode_name(permissions_.mode()));
        }
    }
    if (!auto_allow && goal_unattended_active()) {
        auto_allow = true;
        audit_gate(security::kAuditDecisionAllow, security::kAuditSourceGoal, "unattended_goal");
        LOG_INFO("[goal] unattended auto-approve: " +
                 effective_tc.function_name +
                 (ctx_path.empty() ? std::string{} : " path=" + ctx_path) +
                 (exec_permission
                      ? " sandbox=" + std::string(sandbox::sandbox_mode_name(
                            exec_permission->decision.sandbox))
                      : std::string{}));
    }

    nlohmann::json permission_hook_input = nlohmann::json::object();
    bool permission_request_dispatched = false;
    bool permission_resolution_dispatched = false;
    auto report_permission_resolved =
        [&](const std::string& decision,
            const std::string& source) {
            if (!hook_manager_ || !permission_request_dispatched ||
                permission_resolution_dispatched) {
                return;
            }
            permission_resolution_dispatched = true;
            auto fields = build_hook_common_fields(
                kCodexHookEventPermissionResolved);
            auto payload = build_permission_resolved_hook_payload(
                fields,
                effective_tc.function_name,
                permission_hook_input,
                decision,
                source);
            auto outcome = dispatch_codex_hook(
                kCodexHookEventPermissionResolved,
                effective_tc.function_name,
                payload);
            apply_hook_side_effects(outcome, false);
        };

    if (!auto_allow && hook_manager_) {
        permission_hook_input =
            exec_permission ? exec_permission->arguments :
            parse_tool_args_for_permission_payload(effective_tc.function_arguments);
        permission_request_dispatched = true;
        auto fields = build_hook_common_fields(kCodexHookEventPermissionRequest);
        auto payload = build_tool_hook_payload(
            fields,
            effective_tc.function_name,
            permission_hook_input);
        auto outcome = dispatch_codex_hook(
            kCodexHookEventPermissionRequest,
            effective_tc.function_name,
            payload);
        apply_hook_side_effects(outcome);
        if (outcome.denied || outcome.blocked) {
            const std::string reason = outcome.reason.empty()
                ? "Permission denied by hook."
                : outcome.reason;
            report_permission_resolved("deny", "hook");
            audit_gate(security::kAuditDecisionDeny, security::kAuditSourceHook, "hook_denied");
            return ToolResult{"[Hook denied permission] " + reason, false};
        }
        if (outcome.allowed) {
            auto_allow = true;
            report_permission_resolved("allow", "hook");
            audit_gate(security::kAuditDecisionAllow, security::kAuditSourceHook, "hook_allowed");
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
    if (!auto_allow && headless::active()) {
        if (permissions_.is_dangerous()) {
            auto_allow = true;
            report_permission_resolved("allow", "headless");
            audit_gate(security::kAuditDecisionAllow, security::kAuditSourceHeadless, "headless_yolo");
            LOG_INFO("[headless] yolo auto-approve: " +
                     effective_tc.function_name +
                     (ctx_path.empty() ? std::string{} : " path=" + ctx_path));
        } else {
            LOG_INFO("[headless] denied (needs confirmation): " +
                     effective_tc.function_name);
            report_permission_resolved("deny", "headless");
            audit_gate(security::kAuditDecisionDeny, security::kAuditSourceHeadless, "headless_no_channel");
            return ToolResult{
                "[Headless mode] This tool call requires interactive "
                "user confirmation, which is unavailable in print (-p) "
                "mode; it was denied automatically. Prefer a read-only "
                "alternative and continue. Rerun in an interactive "
                "session to approve this operation.",
                false};
        }
    }

    if (!auto_allow && (prompter_ || callbacks_.on_tool_confirm)) {
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
            : callbacks_.on_tool_confirm(effective_tc.function_name, permission_args);
        if (perm == PermissionResult::Deny) {
            report_permission_resolved("deny", "interactive");
            audit_gate(security::kAuditDecisionDeny, security::kAuditSourceUser,
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
        report_permission_resolved(
            perm == PermissionResult::AlwaysAllow   ? "always_allow"
            : perm == PermissionResult::AllowScoped   ? "allow_scoped"
            : perm == PermissionResult::AllowRemember ? "allow_remember"
                                                      : "allow",
            "interactive");
        audit_gate(
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
            sandbox_runtime_.grant_for_session(grant);
            auto request = sandbox_runtime_.request_for(sandbox::SandboxMode::WorkspaceWrite,
                write_root().empty() ? cwd_ : write_root());
            const auto error = sandbox_runtime_.prepare_request(request);
            if (!error.empty()) {
                sandbox_runtime_.mark_unavailable(error);
                return ToolResult{"[Sandbox unavailable] " + error +
                    ". The scoped grant was recorded but the command was not executed; retry.", false};
            }
            execution_context.exec_sandbox = std::move(request);
            LOG_INFO("[sandbox] scoped grant for session: write " + scoped_root);
            record_audit(security::kAuditCategoryRule, "bash", scoped_root,
                         security::kAuditDecisionAllowScoped, security::kAuditSourceUser,
                         "scoped_grant", sandbox::sandbox_mode_name(sandbox::SandboxMode::WorkspaceWrite),
                         nlohmann::json{{"command", ctx_command}});
        }
        if (perm == PermissionResult::AllowRemember && exec_permission) {
            // D6:写规则文件失败只记日志,本次仍按「允许一次」执行。
            const std::string remember_error = remember_exec_rule(*exec_permission);
            record_audit(security::kAuditCategoryRule, "bash", exec_permission->remember_display(),
                         security::kAuditDecisionAllowRemember, security::kAuditSourceUser,
                         remember_error.empty() ? "remember_rule" : "remember_rule_failed",
                         audit_sandbox,
                         nlohmann::json{{"command", ctx_command}, {"error", remember_error}});
        }
        if ((perm == PermissionResult::AlwaysAllow || perm == PermissionResult::AllowRemember) &&
            permissions_.mode() != PermissionMode::Plan &&
            effective_tc.function_name != "EnterPlanMode" &&
            effective_tc.function_name != "ExitPlanMode") {
            if (exec_permission) {
                if (exec_permission->input.additional_requested && !exec_permission->additional.empty()) {
                    // 额外权限申请的「本次会话允许」记的是权限,不是命令前缀。
                    sandbox_runtime_.grant_for_session(exec_permission->additional);
                    nlohmann::json grant_detail{{"command", ctx_command}};
                    grant_detail["read"] = exec_permission->additional.read;
                    grant_detail["write"] = exec_permission->additional.write;
                    grant_detail["network"] = exec_permission->additional.network;
                    record_audit(security::kAuditCategoryRule, "bash",
                                 exec_permission->additional.write.empty()
                                     ? (exec_permission->additional.read.empty() ? std::string("network")
                                                                                  : exec_permission->additional.read.front())
                                     : exec_permission->additional.write.front(),
                                 security::kAuditDecisionAllowSession, security::kAuditSourceUser,
                                 "session_grant", audit_sandbox, std::move(grant_detail));
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
        auto_allow = true;
    }

    if (exec_permission && !auto_allow) {
        audit_gate(security::kAuditDecisionDeny, security::kAuditSourceNone, "no_confirmation_channel");
        return ToolResult{"[Permission denied] This command requires approval, but no confirmation channel is available.", false};
    }

    // A non-interactive embedding may intentionally omit a
    // prompter while still allowing execution. Close the paired
    // lifecycle event before the tool starts in that case.
    if (!auto_allow && permission_request_dispatched &&
        !permission_resolution_dispatched) {
        report_permission_resolved("allow", "implicit");
    }
    if (!auto_allow) {
        audit_gate(security::kAuditDecisionAllow, security::kAuditSourceNone, "implicit");
    }

    ToolResult tool_result = execute_single_tool(effective_tc.function_name, effective_tc.function_arguments,
                                                 ctx_path, execution_context);
    if (exec_permission && tool_result.metadata.value("sandbox_unavailable", false)) {
        // 只记一行原因:它会进 /sandbox 状态与 system prompt 的
        // `Shell sandbox:` 行,整段工具输出塞进去既难读也浪费上下文。
        std::string reason = tool_result.metadata.value(
            "sandbox_unavailable_reason", std::string{});
        if (reason.empty()) {
            reason = tool_result.output.substr(0, tool_result.output.find('\n'));
        }
        sandbox_runtime_.mark_unavailable(reason);
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
            record_audit(security::kAuditCategorySandbox, "bash", violation.path,
                         security::kAuditDecisionBlocked, security::kAuditSourceSandbox,
                         violation.reason, audit_sandbox,
                         nlohmann::json{{"command", ctx_command}, {"snippet", violation.snippet}});
            last_sandbox_violation_ = std::move(violation);
        } else if (tool_result.success) {
            last_sandbox_violation_.reset();
        }
    }

    if ((effective_tc.function_name == "file_edit" || effective_tc.function_name == "file_write") &&
        !ctx_path.empty() && !tool_result.success) {
        const std::string lower = ascii_lower(tool_result.output);
        if (lower.find("encoding") != std::string::npos ||
            lower.find("old_string") != std::string::npos ||
            lower.find("round-trip") != std::string::npos) {
            recent_safe_edit_failures_[ctx_path] = std::chrono::steady_clock::now();
        }
    }

    if (effective_tc.function_name == "bash" && tool_result.success &&
        command_looks_like_file_write(ctx_command)) {
        for (const auto& [failed_path, when] : recent_safe_edit_failures_) {
            (void)when;
            if (!command_mentions_path(ctx_command, failed_path)) continue;
            auto check = with_text_file_tool_errors(read_text_file_buffer(failed_path, false));
            if (!check.success) {
                tool_result.success = false;
                if (!tool_result.output.empty() && tool_result.output.back() != '\n') {
                    tool_result.output += "\n";
                }
                tool_result.output +=
                    "[Error] Post-command encoding sanity check failed for " +
                    failed_path + ": " + check.error;
            }
        }
    }

    return tool_result;
}

} // namespace acecode
