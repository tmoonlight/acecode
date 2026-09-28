#include "model_step_recorder.hpp"
#include "agent/detail/agent_payloads.hpp"
#include "agent/recovery/provider_error_report.hpp"
#include "agent/event_payload/message_payload.hpp"
#include "session/event_dispatcher.hpp"
#include "session/session_manager.hpp"
#include "session/session_serializer.hpp"
#include "tool/tool_executor.hpp"
#include <utility>

namespace acecode::agent {

using detail::model_step_usage_to_json;
using detail::provider_error_to_json;
using detail::text_tool_call_diagnostic_to_json;

void ModelStepRecorder::start(int step_index) {
    events_.emit(SessionEventKind::ModelStepStart, {{"step_index", step_index}});
}
void ModelStepRecorder::finish(int step_index, std::string reason, const TokenUsage& usage) {
    if (reason.empty()) reason = "unknown";
    events_.emit(SessionEventKind::ModelStepFinish, nlohmann::json{
        {"step_index", step_index},
        {"reason", std::move(reason)},
        {"usage", model_step_usage_to_json(usage)},
    });
}
void ModelStepRecorder::request(SessionManager* session, int step_index,
    const std::shared_ptr<LlmProvider>& provider, const ApiRequestBundle& bundle,
    int context_window) {
    if (!session || !provider) return;
    nlohmann::json messages = nlohmann::json::array();
    for (const auto& message : bundle.messages_with_system) {
        try {
            messages.push_back(
                nlohmann::json::parse(serialize_message(message)));
        } catch (...) {
            messages.push_back(nlohmann::json{
                {"role", message.role},
                {"content", message.content},
            });
        }
    }
    nlohmann::json tools = nlohmann::json::array();
    for (const auto& tool : bundle.tool_defs) {
        const std::string native_name =
            tools_.resolve_model_tool_name_to_native(tool.name);
        tools.push_back(nlohmann::json{
            {"name", tool.name},
            {"native_name", native_name.empty() ? tool.name : native_name},
            {"description", tool.description},
            {"parameters", tool.parameters},
        });
    }
    session->record_trajectory_event(
        "model_request",
        {{"step_index", step_index},
         {"provider", provider->name()},
         {"model", provider->model()},
         {"context_window", context_window},
         {"messages", std::move(messages)},
         {"tools", std::move(tools)},
         {"context_usage_estimate",
          context_usage_breakdown_to_json(
              bundle.context_usage_estimate)},
         {"prompt_diagnostics", bundle.prompt_diag}});
}
void ModelStepRecorder::response(SessionManager* session, int step_index,
    const ProviderCallResult& result, const TokenUsage& usage, std::string status) {
    if (!session) return;
    if (status.empty()) {
        status = result.provider_error_seen ? "error" : "completed";
    }
    nlohmann::json tool_calls = nlohmann::json::array();
    for (const auto& call : result.accumulated.tool_calls) {
        tool_calls.push_back(nlohmann::json{
            {"id", call.id},
            {"name", call.function_name},
            {"arguments", call.function_arguments},
        });
    }
    nlohmann::json payload{
        {"step_index", step_index},
        {"attempt", result.provider_attempt},
        {"content", result.accumulated.content},
        {"reasoning_content", result.accumulated.reasoning_content},
        {"content_parts", result.accumulated.content_parts.is_null()
            ? nlohmann::json::array()
            : result.accumulated.content_parts},
        {"tool_calls", std::move(tool_calls)},
        {"finish_reason", result.accumulated.finish_reason},
        {"usage", model_step_usage_to_json(usage)},
        {"status", std::move(status)},
    };
    if (result.provider_snapshot) {
        payload["provider"] = result.provider_snapshot->name();
        payload["model"] = result.provider_snapshot->model();
    }
    if (result.provider_error_seen ||
        result.provider_error_info.has_error()) {
        payload["error"] = provider_error_to_json(
            result.provider_error_info);
    }
    if (result.accumulated.text_tool_calls.outcome !=
        TextToolCallDiagnostic::Outcome::None) {
        payload["text_tool_calls"] = text_tool_call_diagnostic_to_json(
            result.accumulated.text_tool_calls);
    }
    if (!result.accumulated.content.empty()) {
        ChatMessage id_basis;
        id_basis.role = "assistant";
        id_basis.content = result.accumulated.content;
        payload["message_id"] = web::compute_message_id(id_basis);
    }
    session->record_trajectory_event(
        "model_response", std::move(payload));
}
void ModelStepRecorder::first_output(SessionManager* session, int step_index,
                                     int attempt, const std::string& channel) {
    if (session) session->record_trajectory_event("model_first_output",
        {{"step_index", step_index}, {"attempt", attempt}, {"channel", channel}});
}
} // namespace acecode::agent
