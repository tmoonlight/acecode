#include "agent_hook_bridge.hpp"

#include "agent/hook_bridge/hook_events.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "hooks/hook_manager.hpp"
#include "llm/llm_provider.hpp"
#include "session/session_manager.hpp"
#include "session/session_rewind.hpp"
#include "session/system_notice.hpp"
#include "utils/logger.hpp"
#include "utils/abort_signal.hpp"

#include <sstream>
#include <utility>

namespace acecode::agent {

HookCommonPayloadFields AgentHookBridge::common_fields(
    const std::string& event, SessionManager* session) const {
    return context_.fields(event, session);
}

void AgentHookBridge::clear_context() {
    std::lock_guard<std::mutex> lock(context_mu_);
    request_context_.clear();
}

void AgentHookBridge::assistant_completed(HookManager* manager, SessionManager* session,
    const ChatMessage& assistant_msg,
    const std::shared_ptr<LlmProvider>& provider_snapshot) {
    if (!manager || assistant_msg.role != "assistant") return;

    std::string session_id;
    if (session) {
        session_id = session->current_session_id();
    }

    std::string provider_name;
    std::string model_name;
    if (provider_snapshot) {
        provider_name = provider_snapshot->name();
        model_name = provider_snapshot->model();
    }

    auto payload = build_assistant_message_completed_payload(
        context_.cwd(),
        session_id,
        provider_name,
        model_name,
        assistant_msg);
    manager->dispatch(kHookEventAssistantMessageCompleted, payload, context_.cwd(), &abort_.raw());
}

void AgentHookBridge::apply(const HookAggregateOutcome& outcome,
                                        bool include_additional_context) {
    for (const auto& message : outcome.system_messages) {
        if (!message.empty()) transcript_.dispatch_message("system", "[Hook] " + message, false,
            make_system_notice_metadata("hook_message", {{"text", message}}), nlohmann::json::array());
    }
    if (include_additional_context) {
        std::lock_guard<std::mutex> lock(context_mu_);
        for (const auto& context : outcome.additional_context) {
            if (!context.empty()) request_context_.push_back(context);
        }
    }
    for (const auto& diagnostic : outcome.diagnostics) {
        if (diagnostic.severity == HookDiagnosticSeverity::Error ||
            diagnostic.severity == HookDiagnosticSeverity::Warning) {
            LOG_WARN("[hooks] " + diagnostic.code + " " + diagnostic.message);
        }
    }
}

std::string AgentHookBridge::drain_context() {
    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> lock(context_mu_);
        pending.swap(request_context_);
    }
    if (pending.empty()) return {};
    std::ostringstream oss;
    oss << "<hook_context>\n";
    for (const auto& context : pending) {
        if (!context.empty()) oss << context << "\n";
    }
    oss << "</hook_context>";
    return oss.str();
}

HookAggregateOutcome AgentHookBridge::dispatch(HookManager* manager,
    const std::string& event_name,
    const std::string& matcher_value,
    const nlohmann::json& payload) {
    if (!manager) return {};
    HookDispatchRequest request;
    request.event_name = event_name;
    request.matcher_value = matcher_value;
    request.cwd = context_.cwd();
    request.payload = payload.is_object() ? payload : nlohmann::json::object();
    // Session lifecycle hooks are outside a foreground turn. All synchronous
    // turn hooks borrow the same cancellation channel as model/tool execution.
    if (event_name != kCodexHookEventSessionStart && event_name != kCodexHookEventSessionTitleChanged) {
        request.abort_flag = &abort_.raw();
    }
    return manager->dispatch_codex(request);
}

void AgentHookBridge::session_start(HookManager* manager, SessionManager* session, const std::string& source) {
    if (!manager) return;
    auto fields = common_fields(kCodexHookEventSessionStart, session);
    auto payload = build_session_start_hook_payload(fields, source);
    auto outcome = dispatch(manager, kCodexHookEventSessionStart, source, payload);
    apply(outcome);
}

void AgentHookBridge::session_title_changed(HookManager* manager, SessionManager* session,
    const std::string& title,
    const std::string& source,
    const std::string& title_source) {
    if (!manager) return;
    auto fields = common_fields(kCodexHookEventSessionTitleChanged, session);
    auto payload = build_session_title_changed_hook_payload(
        fields, title, source, title_source);
    (void)dispatch(manager,
        kCodexHookEventSessionTitleChanged, source, payload);
}

bool AgentHookBridge::continue_from_stop(
    HookManager* manager, SessionManager* session, const std::string& last_assistant_message) {
    if (!manager) return false;
    auto fields = common_fields(kCodexHookEventStop, session);
    auto payload = build_stop_hook_payload(fields, stop_active_, last_assistant_message);
    auto outcome = dispatch(manager, kCodexHookEventStop, std::string{}, payload);
    apply(outcome);
    if (outcome.continue_false) {
        stop_active_ = false;
        return false;
    }
    if ((outcome.blocked || outcome.denied) && !stop_active_ && !outcome.reason.empty()) {
        stop_active_ = true;
        ChatMessage message;
        message.role = "user";
        message.content = outcome.reason;
        message.metadata = nlohmann::json{
            {"hidden_hook_stop_continuation", true}, {"hidden_goal_context", true}};
        ensure_user_message_identity(message);
        history_.append(message);
        if (session) session->on_message(message);
        return true;
    }
    stop_active_ = false;
    return false;
}

} // namespace acecode::agent
