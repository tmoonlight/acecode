#include "agent/agent_loop.hpp"
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

namespace acecode {

void AgentLoop::extract_context(const ToolCall& tc, std::string& ctx_path, std::string& ctx_command) {
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

ToolResult AgentLoop::execute_single_tool(const std::string& tool_name, const std::string& tool_args, const std::string& ctx_path, const ToolContext& tool_ctx) {
    if (!ctx_path.empty() && tool_name != "bash") {
        std::string path_error = path_validation_error(tool_name, ctx_path);
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

std::optional<ToolResult> AgentLoop::maybe_guard_tool(ToolBatchState& batch, const ToolCall& tc) {
    std::lock_guard<std::mutex> lk(batch.doom_guard_mu);
    return batch.doom_guard.maybe_guard(tc);
}

void AgentLoop::record_doom_guard_result(ToolBatchState& batch, const ToolCall& tc, const ToolResult& result) {
    std::lock_guard<std::mutex> lk(batch.doom_guard_mu);
    batch.doom_guard.record_result(tc, result);
}

} // namespace acecode
