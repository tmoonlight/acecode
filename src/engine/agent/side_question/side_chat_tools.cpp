#include "side_chat_tools.hpp"

#include "agent/approval/path_access_policy.hpp"
#include "agent/boundary/workspace_boundary.hpp"
#include "agent/tool_exec/tool_invoker.hpp"
#include "agent/tool_exec/tool_session_host.hpp"
#include "permissions/permissions.hpp"
#include "tool/mtime_tracker.hpp"
#include "utils/logger.hpp"

#include <unordered_set>

namespace acecode::agent {
namespace {

// PathAccessPolicy 需要的会话宿主。侧边对话只读,从不切换工作目录。
class SideChatToolHost final : public ToolSessionHost {
public:
    SideChatToolHost(WorkspaceBoundary& boundary, SessionManager* session)
        : boundary_(boundary), session_(session) {}
    std::string cwd() const override { return boundary_.cwd(); }
    std::string write_root() const override { return boundary_.write_root(session_); }
    std::vector<std::string> writable_workspace_folders() const override {
        return boundary_.writable_workspace_folders(session_);
    }
    bool path_in_workspace_folders(const std::string& path) const override {
        return boundary_.path_in_workspace_folders(path, session_);
    }
    void switch_cwd(const std::string&) override {}

private:
    WorkspaceBoundary& boundary_;
    SessionManager* session_;
};

std::string available_tool_list(const std::vector<ToolDef>& definitions) {
    std::string names;
    for (const auto& definition : definitions) {
        if (!names.empty()) names += ", ";
        names += definition.name;
    }
    return names;
}

} // namespace

SideChatToolset build_side_chat_toolset(ToolExecutor& tools,
                                        PermissionManager& permissions,
                                        WorkspaceBoundary& boundary,
                                        SessionManager* session,
                                        const ToolCapabilityPolicy& session_policy) {
    std::unordered_set<std::string> allowed;
    for (const char* name : kSideChatReadOnlyTools) {
        if (tools.is_read_only(name) && tools.is_allowed(name, &session_policy)) {
            allowed.insert(name);
        }
    }
    ToolCapabilityPolicy side_policy;
    side_policy.builtin_tools = allowed;
    side_policy.mcp_servers = std::unordered_set<std::string>{};

    SideChatToolset toolset;
    toolset.definitions = tools.get_model_tool_definitions(&side_policy);
    if (toolset.definitions.empty()) return toolset;
    const std::string available = available_tool_list(toolset.definitions);

    toolset.native_name = [&tools](const std::string& model_name) {
        return tools.resolve_model_tool_name_to_native(model_name);
    };
    toolset.execute = [&tools, &permissions, &boundary, session, side_policy, allowed, available](
                          const ToolCall& call, const std::atomic<bool>* abort_flag) -> ToolResult {
        const std::string native = tools.resolve_model_tool_name_to_native(call.function_name);
        if (allowed.count(native) == 0) {
            return ToolResult{"[Error] " + call.function_name +
                " is not available in this read-only side chat. Available tools: " +
                available + ".", false};
        }
        std::string path;
        std::string command;
        ToolInvoker::extract_context(call, path, command);
        if (!permissions.should_auto_allow(native, true, path)) {
            return ToolResult{"[Permission denied] A configured rule requires confirmation for "
                "this read, which the side chat cannot ask for.", false};
        }
        if (!path.empty()) {
            SideChatToolHost host(boundary, session);
            PathAccessPolicy paths(tools, permissions, boundary, host, session);
            const std::string path_error = paths.path_validation_error(native, path);
            if (!path_error.empty()) return ToolResult{"[Error] " + path_error, false};
            if (!permissions.is_dangerous() &&
                permissions.mode() != PermissionMode::Yolo &&
                boundary.is_dangerous_path(path)) {
                return ToolResult{"[Permission denied] Reading " + path +
                    " requires user confirmation, which the side chat cannot ask for.", false};
            }
        }
        ToolContext context;
        context.cwd = boundary.cwd();
        context.abort_flag = abort_flag;
        context.capability_policy = side_policy;
        LOG_INFO("[side-chat] read-only tool " + native);
        MtimeTracker::DetachedReadScope detached_read;
        return tools.execute(native, call.function_arguments, context);
    };
    return toolset;
}

} // namespace acecode::agent
