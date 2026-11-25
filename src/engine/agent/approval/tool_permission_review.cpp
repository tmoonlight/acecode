#include "tool_permission_gate.hpp"

#include "agent/approval/session_exec_security.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "agent/hook_bridge/tool_hook_bridge.hpp"
#include "llm/tool_protocol_names.hpp"
#include "permissions/auto_review_prompt.hpp"
#include "permissions/permissions.hpp"
#include "utils/logger.hpp"

#include <utility>

// 自动模式（试验）在审批门里的那一步(openspec add-auto-review-mode,design D2 ~ D5)。
// 它是审批门的成员函数,只因为审批门的主文件已经够长才分到这个文件;
// 调用点只有审批入口一处。

namespace acecode::agent {

std::optional<ToolResult> ToolPermissionGate::review_pending_call(
    const ToolCall& call, std::size_t tool_index, const ProgressEmitter& emit_progress,
    const std::optional<sandbox::ExecPermission>& exec_permission, bool human_only,
    PermissionAuditScope& audit, PermissionHookSession& permission_session,
    bool& auto_allow, nlohmann::json& review_note, bool& limit_fallback) {
    auto& denials = security_.review_denials();
    // 切出本模式再切回来,计数与锁定从头开始。
    denials.sync_mode_generation(permissions_.mode_generation());

    const bool headless = security_.environment().headless();
    const bool unattended = goal_.unattended_active(session_manager_);
    const bool attended = !headless && !unattended && confirmation_.available();
    // 给主模型看的文案用模型侧工具名。
    const std::string tool_label = model_tool_name_for_native(call.function_name);
    const char* absent_source = unattended ? security::kAuditSourceGoal
        : headless ? security::kAuditSourceHeadless : security::kAuditSourceNone;
    const auto refuse = [](std::string text, nlohmann::json meta) {
        ToolResult result{std::move(text), false};
        result.metadata["auto_review"] = std::move(meta);
        return result;
    };

    if (human_only) {
        if (attended) return std::nullopt;
        // 裁决都不能放行的东西,无人可问时只能拒绝(goal 的一律放行也不走)。
        permission_session.resolve("deny", "auto_review");
        audit.record(security::kAuditDecisionDeny, absent_source, "review_needs_user");
        return refuse(auto_review::needs_user_result_text(tool_label), {{"needs_user", true}});
    }

    denials.unlock_if_new_user_turn(review_.user_turn_anchor());
    if (denials.locked()) {
        permission_session.resolve("deny", "classifier");
        audit.record(security::kAuditDecisionDeny, security::kAuditSourceClassifier,
                     "review_denial_limit");
        return refuse(auto_review::locked_result_text(tool_label), {{"locked", true}});
    }

    emit_progress("permission_reviewing", "正在进行安全检查",
        call.function_name, call.function_name, call.id, static_cast<int>(tool_index), true);
    std::string execution;
    if (exec_permission) {
        switch (exec_permission->decision.sandbox) {
            case sandbox::SandboxMode::FullAccess:
                execution = "with full access (no sandbox)";
                break;
            case sandbox::SandboxMode::WorkspaceWrite:
                execution = "inside a sandbox limited to the workspace";
                break;
            default:
                execution = "inside a read-only sandbox";
                break;
        }
    }
    const ReviewOutcome outcome = review_.review(call, execution);
    const nlohmann::json detail = {{"review_model", outcome.model},
                                   {"review_ms", outcome.duration_ms},
                                   {"review_reason", outcome.reason}};

    switch (outcome.kind) {
        case ReviewOutcome::Kind::Allowed:
            denials.record_allow();
            auto_allow = true;
            permission_session.resolve("allow", "classifier");
            audit.record(security::kAuditDecisionAllow, security::kAuditSourceClassifier,
                         outcome.stage, detail);
            emit_progress("tool_running", "正在调用工具 " + call.function_name,
                call.function_name, call.function_name, call.id, static_cast<int>(tool_index), true);
            return std::nullopt;

        case ReviewOutcome::Kind::Aborted:
            permission_session.resolve("deny", "interrupted");
            audit.record(security::kAuditDecisionDeny, security::kAuditSourceNone,
                         "review_interrupted");
            return refuse("[Auto mode] " + tool_label + " was not run: the turn was interrupted.",
                          {{"interrupted", true}});

        case ReviewOutcome::Kind::NoVerdict:
            // 没有结果不是拦截,不计数。有人在场就问人;无人可问时拒绝并说明可重试。
            if (attended) {
                review_note = {{"why", "no_verdict"}, {"reason", outcome.reason}};
                return std::nullopt;
            }
            permission_session.resolve("deny", "classifier");
            audit.record(security::kAuditDecisionDeny, absent_source, "review_no_verdict", detail);
            return refuse(auto_review::unavailable_result_text(tool_label, outcome.reason),
                          {{"no_verdict", true}, {"reason", outcome.reason}});

        case ReviewOutcome::Kind::Blocked:
            break;
    }

    // 拦下。之前已经达到上限且有人在场:这一次改为问人,把裁决的理由带给确认框。
    if (attended && denials.limit_reached()) {
        review_note = {{"why", "denial_limit"}, {"reason", outcome.reason}};
        limit_fallback = true;
        return std::nullopt;
    }
    const bool limit_now = denials.record_block();
    permission_session.resolve("deny", "classifier");
    audit.record(security::kAuditDecisionDeny, security::kAuditSourceClassifier, outcome.stage, detail);
    std::string text = auto_review::blocked_result_text(tool_label, outcome.reason);
    nlohmann::json meta = {{"blocked", true}, {"reason", outcome.reason}, {"stage", outcome.stage}};
    if (limit_now && !attended) {
        // 无人可问:锁定,此后需要裁决的调用直接拒绝、不再调裁决模型;goal 一并停住。
        denials.lock(review_.user_turn_anchor());
        if (unattended) goal_.block_after_review_denials(session_manager_);
        text += "\n" + auto_review::locked_result_text(tool_label);
        meta["locked"] = true;
        LOG_WARN("[auto-review] denial limit reached with nobody to ask; locked until the next user message");
    }
    return refuse(std::move(text), std::move(meta));
}

} // namespace acecode::agent
