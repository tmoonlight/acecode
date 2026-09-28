#include "agent/agent_loop.hpp"
#include "agent/hook_bridge/hook_events.hpp"
#include "hooks/hook_config.hpp"
#include "hooks/hook_manager.hpp"
#include "hooks/hook_runtime.hpp"
#include "llm/tool_protocol_names.hpp"
#include "permissions/shell_write_guard.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/system_notice.hpp"
#include "session/turn_timing.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"
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

void AgentLoop::dispatch_assistant_completed_hook(
    const ChatMessage& assistant_msg,
    const std::shared_ptr<LlmProvider>& provider_snapshot) {
    if (!hook_manager_ || assistant_msg.role != "assistant") return;

    std::string session_id;
    if (session_manager_) {
        session_id = session_manager_->current_session_id();
    }

    std::string provider_name;
    std::string model_name;
    if (provider_snapshot) {
        provider_name = provider_snapshot->name();
        model_name = provider_snapshot->model();
    }

    auto payload = build_assistant_message_completed_payload(
        cwd_,
        session_id,
        provider_name,
        model_name,
        assistant_msg);
    hook_manager_->dispatch(kHookEventAssistantMessageCompleted, payload, cwd_);
}

HookCommonPayloadFields AgentLoop::build_hook_common_fields(
    const std::string& event_name) const {
    HookCommonPayloadFields fields;
    fields.cwd = cwd_;
    fields.hook_event_name = event_name;
    fields.permission_mode = PermissionManager::mode_name(permissions_.mode());
    if (session_manager_) {
        fields.session_id = session_manager_->current_session_id();
        if (!fields.session_id.empty()) {
            fields.transcript_path = SessionStorage::session_path(
                SessionStorage::get_project_dir(cwd_), fields.session_id);
        }
    }
    if (provider_accessor_) {
        auto provider = provider_accessor_();
        if (provider) fields.model = provider->model();
    }
    return fields;
}

void AgentLoop::apply_hook_side_effects(const HookAggregateOutcome& outcome,
                                        bool include_additional_context) {
    for (const auto& message : outcome.system_messages) {
        if (!message.empty()) dispatch_message("system", "[Hook] " + message, false,
            make_system_notice_metadata("hook_message", {{"text", message}}));
    }
    if (include_additional_context) {
        for (const auto& context : outcome.additional_context) {
            if (!context.empty()) hook_request_context_.push_back(context);
        }
    }
    for (const auto& diagnostic : outcome.diagnostics) {
        if (diagnostic.severity == HookDiagnosticSeverity::Error ||
            diagnostic.severity == HookDiagnosticSeverity::Warning) {
            LOG_WARN("[hooks] " + diagnostic.code + " " + diagnostic.message);
        }
    }
}

std::string AgentLoop::drain_hook_request_context() {
    if (hook_request_context_.empty()) return {};
    std::ostringstream oss;
    oss << "<hook_context>\n";
    for (const auto& context : hook_request_context_) {
        if (!context.empty()) oss << context << "\n";
    }
    oss << "</hook_context>";
    hook_request_context_.clear();
    return oss.str();
}

HookAggregateOutcome AgentLoop::dispatch_codex_hook(
    const std::string& event_name,
    const std::string& matcher_value,
    const nlohmann::json& payload) {
    if (!hook_manager_) return {};
    HookDispatchRequest request;
    request.event_name = event_name;
    request.matcher_value = matcher_value;
    request.cwd = cwd_;
    request.payload = payload.is_object() ? payload : nlohmann::json::object();
    return hook_manager_->dispatch_codex(request);
}

void AgentLoop::dispatch_session_start_hook(const std::string& source) {
    if (!hook_manager_) return;
    auto fields = build_hook_common_fields(kCodexHookEventSessionStart);
    auto payload = build_session_start_hook_payload(fields, source);
    auto outcome = dispatch_codex_hook(kCodexHookEventSessionStart, source, payload);
    apply_hook_side_effects(outcome);
}

void AgentLoop::dispatch_session_title_changed_hook(
    const std::string& title,
    const std::string& source,
    const std::string& title_source) {
    if (!hook_manager_) return;
    auto fields = build_hook_common_fields(kCodexHookEventSessionTitleChanged);
    auto payload = build_session_title_changed_hook_payload(
        fields, title, source, title_source);
    (void)dispatch_codex_hook(
        kCodexHookEventSessionTitleChanged, source, payload);
}

} // namespace acecode
