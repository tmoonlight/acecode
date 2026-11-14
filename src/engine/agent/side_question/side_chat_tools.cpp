#include "side_chat_tools.hpp"

#include "agent/approval/path_access_policy.hpp"
#include "agent/boundary/workspace_boundary.hpp"
#include "agent/tool_exec/tool_invoker.hpp"
#include "agent/tool_exec/tool_session_host.hpp"
#include "permissions/permissions.hpp"
#include "tool/mtime_tracker.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"
#include "memory/secret_redaction.hpp"

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

namespace {

ToolDef side_chat_send_to_main_definition() {
    return ToolDef{
        kSideChatSendToMainTool,
        "Ask the agent of the main session to do something, or pass it a message. "
        "Use it when the user wants something this side chat cannot do itself "
        "(creating or editing files, running commands, continuing the task), or "
        "asks you to pass something on. The main agent cannot see this side "
        "conversation, so write a self-contained request with every detail it "
        "needs. The user reviews and confirms the message before it is sent. If "
        "the main agent is working, it reads the message at its next step; "
        "otherwise the message starts a new turn.",
        nlohmann::json{
            {"type", "object"},
            {"properties", {{"message", {{"type", "string"},
                                         {"description", "The self-contained message to deliver."}}}}},
            {"required", nlohmann::json::array({"message"})},
        }};
}

std::string send_to_main_message(const std::string& arguments_json) {
    try {
        const auto args = nlohmann::json::parse(arguments_json);
        const auto it = args.is_object() ? args.find("message") : args.end();
        if (args.is_object() && it != args.end() && it->is_string()) return it->get<std::string>();
    } catch (...) {}
    return {};
}

} // namespace

SideChatToolset build_side_chat_toolset(ToolExecutor& tools,
                                        PermissionManager& permissions,
                                        WorkspaceBoundary& boundary,
                                        SessionManager* session,
                                        const ToolCapabilityPolicy& session_policy,
                                        const SideChatMainSessionSender& send_to_main,
                                        const std::function<bool(const std::string&)>& extra_read_denied) {
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
    if (send_to_main) {
        toolset.definitions.push_back(side_chat_send_to_main_definition());
        toolset.can_send_to_main = true;
    }
    if (toolset.definitions.empty()) return toolset;
    const std::string available = available_tool_list(toolset.definitions);

    toolset.native_name = [&tools](const std::string& model_name) {
        return tools.resolve_model_tool_name_to_native(model_name);
    };
    toolset.execute = [&tools, &permissions, &boundary, session, side_policy, allowed, available,
                       send_to_main, extra_read_denied](
                          const ToolCall& call, const std::atomic<bool>* abort_flag) -> ToolResult {
        if (send_to_main && call.function_name == kSideChatSendToMainTool) {
            const std::string message = send_to_main_message(call.function_arguments);
            if (message.find_first_not_of(" \t\r\n") == std::string::npos) {
                return ToolResult{"[Error] send_to_main_session needs a non-empty message.", false};
            }
            if (message.size() > kMaxSideQuestionBytes) {
                return ToolResult{"[Error] The message is too long; keep it under 16000 bytes.", false};
            }
            return send_to_main(call, message, abort_flag);
        }
        const std::string native = tools.resolve_model_tool_name_to_native(call.function_name);
        if (allowed.count(native) == 0) {
            return ToolResult{"[Error] " + call.function_name +
                " is not available in this read-only side chat. Available tools: " +
                available + ".", false};
        }
        std::string path;
        std::string command;
        ToolInvoker::extract_context(call, path, command);
        auto deny_read = [extra_read_denied, cwd = boundary.cwd()](const std::string& candidate) {
            if (!extra_read_denied) return false;
            if (extra_read_denied(candidate)) return true;
            auto resolved = path_from_utf8(candidate);
            if (resolved.is_relative()) resolved = path_from_utf8(cwd) / resolved;
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(resolved, ec);
            return ec || extra_read_denied(path_to_utf8(canonical));
        };
        if (extra_read_denied && !path.empty() && deny_read(path))
            return ToolResult{"[Permission denied] This file is not available to guests.", false};
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
        if (extra_read_denied) context.extra_read_denied = deny_read;
        context.abort_flag = abort_flag;
        context.capability_policy = side_policy;
        LOG_INFO("[side-chat] read-only tool " + native);
        MtimeTracker::DetachedReadScope detached_read;
        auto result = tools.execute(native, call.function_arguments, context);
        if (extra_read_denied) result.output = redact_secrets(result.output).text;
        return result;
    };
    return toolset;
}

} // namespace acecode::agent
