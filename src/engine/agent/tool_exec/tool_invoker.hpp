#pragma once
#include "tool_batch_types.hpp"
#include "tool/tool_executor.hpp"

namespace acecode::agent {
class PathAccessPolicy;
class ToolPermissionGate;

class ToolInvoker {
public:
    ToolInvoker(ToolExecutor& tools, PathAccessPolicy& paths, ToolPermissionGate& gate,
        std::vector<std::string> model_tool_names)
        : tools_(tools), paths_(paths), gate_(gate),
          current_request_model_tool_names_(std::move(model_tool_names)) {}
    static void extract_context(const ToolCall& call, std::string& path, std::string& command);
    ToolResult invoke(ToolBatchState& batch, const ToolCall& call,
        const ToolContext& context, const std::string& path, const std::string& command,
        std::size_t index, bool needs_approval);
private:
    ToolResult execute_single_tool(const std::string& name, const std::string& args,
        const std::string& path, const ToolContext& context);
    ToolExecutor& tools_;
    PathAccessPolicy& paths_;
    ToolPermissionGate& gate_;
    std::vector<std::string> current_request_model_tool_names_;
};
} // namespace acecode::agent
