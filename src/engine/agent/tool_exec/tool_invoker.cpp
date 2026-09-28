#include "tool_invoker.hpp"
#include "agent/approval/path_access_policy.hpp"
#include "agent/approval/tool_permission_gate.hpp"
#include "agent/guards/doom_guard.hpp"
#include "agent/tool_exec/tool_batch_types.hpp"
#include "computer_use/runtime.hpp"
#include "permissions/shell_write_guard.hpp"
#include "session/session_storage.hpp"
#include "session/task_suggestion_store.hpp"
#include "tool/tool_errors.hpp"
#include "utils/logger.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode::agent {

void ToolInvoker::extract_context(const ToolCall& tc, std::string& ctx_path, std::string& ctx_command) {
    try {
        auto args_json = nlohmann::json::parse(tc.function_arguments);
        if (args_json.contains("file_path") && args_json["file_path"].is_string()) {
            ctx_path = args_json["file_path"].get<std::string>();
        } else if (args_json.contains("image_path") &&
                   args_json["image_path"].is_string()) {
            ctx_path = args_json["image_path"].get<std::string>();
        } else if (args_json.contains("path") && args_json["path"].is_string()) {
            ctx_path = args_json["path"].get<std::string>();
        }
        if (args_json.contains("command") && args_json["command"].is_string()) {
            ctx_command = args_json["command"].get<std::string>();
        }
    } catch (...) {}
}

ToolResult ToolInvoker::execute_single_tool(const std::string& tool_name, const std::string& tool_args, const std::string& ctx_path, const ToolContext& tool_ctx) {
    if (!ctx_path.empty() && tool_name != "bash") {
        std::string path_error = paths_.path_validation_error(tool_name, ctx_path);
        if (!path_error.empty()) {
            LOG_WARN("Path validation failed: " + path_error);
            return ToolResult{"[Error] " + path_error, false};
        }
    }
    if (tools_.has_tool(tool_name)) {
        LOG_DEBUG("Executing tool: " + tool_name);
        try {
            ToolResult result = tools_.execute(tool_name, tool_args, tool_ctx);
            LOG_INFO("Tool result: success=" + std::string(result.success ? "true" : "false") +
                     " output=" + log_truncate(result.output, 300));
            return result;
        } catch (const std::exception& e) {
            LOG_ERROR("Tool execution error: " + std::string(e.what()));
            return ToolResult{"[Error] Tool execution failed: " + std::string(e.what()), false};
        }
    } else {
        LOG_WARN("Unknown tool: " + tool_name);
        return ToolResult{
            ToolErrors::unknown_tool(tool_name,
                                     current_request_model_tool_names_),
            false};
    }
}


ToolResult ToolInvoker::invoke(ToolBatchState& batch, const ToolCall& call,
    const ToolContext& context, const std::string& path, const std::string& command,
    std::size_t index, bool needs_approval) {
    const auto* policy = context.capability_policy ? &*context.capability_policy : nullptr;
    if (tools_.is_denied_by_policy(call.function_name, policy)) {
        return {"[Error] Tool denied by the active expert capability policy: " +
            call.function_name, false};
    }
    if (auto denied = batch.doom_guard.maybe_guard(call)) return *denied;
    if (!needs_approval) {
        return execute_single_tool(call.function_name, call.function_arguments, path, context);
    }
    auto verdict = gate_.decide(call, context, path, command, index, batch.emit_progress);
    if (verdict.denial) return std::move(*verdict.denial);
    auto result = execute_single_tool(
        call.function_name, call.function_arguments, path, verdict.execution_context);
    gate_.observe_result(verdict, call, path, command, result);
    return result;
}
} // namespace acecode::agent
