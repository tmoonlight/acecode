#include "tool_hook_bridge.hpp"

#include "agent/approval/permission_payloads.hpp"
#include "agent/hook_bridge/agent_hook_bridge.hpp"
#include "session/session_manager.hpp"
#include "utils/time.hpp"

namespace acecode::agent {

using detail::parse_tool_args_for_permission_payload;
using utils::now_epoch_ms;

std::optional<ToolResult> ToolHookBridge::before(
    HookManager* manager, SessionManager* session, ToolCall& tc, std::size_t tool_index) {
    if (!manager) return std::nullopt;
    auto fields = hooks_.common_fields(kCodexHookEventPreToolUse, session);
    auto payload = build_tool_hook_payload(
        fields,
        tc.function_name,
        parse_tool_args_for_permission_payload(tc.function_arguments));
    auto outcome = hooks_.dispatch(manager,
        kCodexHookEventPreToolUse, tc.function_name, payload);
    hooks_.apply(outcome);
    if (outcome.updated_input.has_value()) {
        const auto& updated = *outcome.updated_input;
        tc.function_arguments = updated.is_string()
            ? updated.get<std::string>()
            : updated.dump();
    }
    if (outcome.denied || outcome.blocked) {
        const std::string reason = outcome.reason.empty()
            ? "Tool execution denied by hook."
            : outcome.reason;
        ToolResult denied_result{
            "[Hook denied tool execution] " + reason, false};
        if (session) {
            nlohmann::json args_payload;
            try {
                args_payload = nlohmann::json::parse(
                    tc.function_arguments);
            } catch (...) {
                args_payload = tc.function_arguments;
            }
            const auto timestamp_ms = now_epoch_ms();
            session->record_trajectory_event(
                "tool_start",
                {{"tool", tc.function_name},
                 {"args", args_payload},
                 {"tool_call_id", tc.id},
                 {"tool_index", static_cast<int>(tool_index)},
                 {"started_at_ms", timestamp_ms}},
                timestamp_ms);
            session->record_trajectory_event(
                "tool_end",
                {{"tool", tc.function_name},
                 {"tool_call_id", tc.id},
                 {"tool_index", static_cast<int>(tool_index)},
                 {"success", false},
                 {"output", denied_result.output},
                 {"started_at_ms", timestamp_ms},
                 {"completed_at_ms", timestamp_ms},
                 {"duration_ms", 0},
                 {"failure_stage", "pre_tool_hook"}},
                timestamp_ms);
        }
        return denied_result;
    }
    return std::nullopt;
}

void ToolHookBridge::after(
    HookManager* manager, SessionManager* session, const ToolCall& tc, ToolResult& result) {
    if (!manager) return;
    nlohmann::json response = {
        {"success", result.success},
        {"output", result.output},
    };
    auto fields = hooks_.common_fields(kCodexHookEventPostToolUse, session);
    auto payload = build_tool_hook_payload(
        fields,
        tc.function_name,
        parse_tool_args_for_permission_payload(tc.function_arguments),
        response);
    auto outcome = hooks_.dispatch(manager,
        kCodexHookEventPostToolUse, tc.function_name, payload);
    hooks_.apply(outcome);
    if (outcome.replacement_output.has_value()) {
        result.output = *outcome.replacement_output;
        if (outcome.blocked || outcome.continue_false) result.success = false;
    } else if ((outcome.blocked || outcome.continue_false) && !outcome.reason.empty()) {
        result.output = outcome.reason;
        result.success = false;
    }
}

HookAggregateOutcome ToolHookBridge::permission_request(
    HookManager* manager, SessionManager* session, const std::string& tool, const nlohmann::json& input) {
    auto fields = hooks_.common_fields(kCodexHookEventPermissionRequest, session);
    auto payload = build_tool_hook_payload(fields, tool, input);
    auto outcome = hooks_.dispatch(manager, kCodexHookEventPermissionRequest, tool, payload);
    hooks_.apply(outcome);
    return outcome;
}

void ToolHookBridge::permission_resolved(
    HookManager* manager, SessionManager* session, const std::string& tool, const nlohmann::json& input,
    const std::string& decision, const std::string& source) {
    auto fields = hooks_.common_fields(kCodexHookEventPermissionResolved, session);
    auto payload = build_permission_resolved_hook_payload(fields, tool, input, decision, source);
    auto outcome = hooks_.dispatch(manager, kCodexHookEventPermissionResolved, tool, payload);
    hooks_.apply(outcome, false);
}

PermissionHookSession::~PermissionHookSession() noexcept(false) {
    if (std::uncaught_exceptions() == exceptions_ && pending()) resolve("allow", "implicit");
}

HookAggregateOutcome PermissionHookSession::request(nlohmann::json input) {
    input_ = std::move(input);
    requested_ = true;
    return bridge_.permission_request(manager_, session_, tool_, input_);
}

void PermissionHookSession::resolve(const std::string& decision, const std::string& source) {
    if (!manager_ || !requested_ || resolved_) return;
    resolved_ = true;
    bridge_.permission_resolved(manager_, session_, tool_, input_, decision, source);
}

} // namespace acecode::agent
