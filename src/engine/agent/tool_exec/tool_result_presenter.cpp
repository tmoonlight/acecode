#include "agent/agent_loop.hpp"
#include "llm/tool_protocol_names.hpp"
#include "permissions/interaction_mode.hpp"
#include "permissions/shell_write_guard.hpp"
#include "session/output_attachments.hpp"
#include "session/session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "tool/ask_user_question_tool.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode {

void AgentLoop::materialize_result_attachments(ToolResult& result) {
    if (!result.has_attachments()) return;
    if (!session_manager_) {
        result.attachment_warnings.push_back(
            "active session required for output attachments");
        result.attachments = nlohmann::json::array();
        return;
    }
    const std::string session_id = session_manager_->ensure_active_session_id();
    const std::string project_dir = SessionStorage::get_project_dir(cwd_);
    auto materialized = materialize_output_attachments(
        result.attachments,
        project_dir,
        session_id,
        [this](const std::string& path) {
            std::string error = path_validator_.validate(path);
            if (!error.empty() && path_in_workspace_folders(path)) error.clear();
            return error;
        },
        cwd_);
    result.attachments = std::move(materialized.attachments);
    result.attachment_warnings.insert(
        result.attachment_warnings.end(),
        materialized.warnings.begin(),
        materialized.warnings.end());
}

void AgentLoop::dispatch_tool_result_display(const ToolCall& tc, const ToolResult& result) {
    std::string display_output = result.output;
    std::string ask_display =
        format_ask_user_question_result_display(result.metadata);
    if (!ask_display.empty()) {
        display_output = std::move(ask_display);
    }
    std::string attachment_fallback =
        output_attachments_fallback_text(result.attachments);
    if (!attachment_fallback.empty()) {
        if (!display_output.empty() && display_output.back() != '\n') {
            display_output.push_back('\n');
        }
        display_output += attachment_fallback;
    }
    dispatch_message("tool_result", display_output, true);
    if (callbacks_.on_tool_result) {
        ChatMessage call_msg;
        call_msg.role = "tool_call";
        call_msg.content = "[Tool: " + tc.function_name + "] " + tc.function_arguments;
        call_msg.display_override =
            ToolExecutor::build_tool_call_preview(tc.function_name, tc.function_arguments);
        callbacks_.on_tool_result(call_msg, tc.function_name, result);
    }
}

} // namespace acecode
