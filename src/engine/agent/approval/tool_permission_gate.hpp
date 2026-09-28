#pragma once
#include "exec_permission_gate.hpp"
#include "path_access_policy.hpp"
#include "permission_audit_scope.hpp"
#include "permission_confirmation.hpp"
#include "agent/turn/turn_types.hpp"

namespace acecode { class HookManager; }
namespace acecode::agent {
class ToolHookBridge;

struct PermissionVerdict {
    ToolContext execution_context;
    std::optional<sandbox::ExecPermission> exec_permission;
    std::string audit_sandbox;
    std::optional<ToolResult> denial;

    PermissionVerdict(ToolResult denied) : denial(std::move(denied)) {}
    PermissionVerdict(ToolContext context, std::optional<sandbox::ExecPermission> exec,
                      std::string sandbox)
        : execution_context(std::move(context)), exec_permission(std::move(exec)),
          audit_sandbox(std::move(sandbox)) {}
};

// The single ordered approval entry. decide() never executes a tool.
class ToolPermissionGate {
public:
    ToolPermissionGate(ToolExecutor& tools, PermissionManager& permissions,
        WorkspaceBoundary& boundary, SessionExecSecurity& security, ToolSessionHost& host,
        GoalRuntime& goal, ToolHookBridge& hooks, ExecPermissionGate& exec,
        PathAccessPolicy& paths, PermissionConfirmation& confirmation,
        SessionManager* session, HookManager* manager)
        : tools_(tools), permissions_(permissions), boundary_(boundary), security_(security),
          host_(host), goal_(goal), tool_hooks_(hooks), exec_(exec), paths_(paths),
          confirmation_(confirmation), session_manager_(session), hook_manager_(manager) {}
    PermissionVerdict decide(const ToolCall& call, const ToolContext& context,
        const std::string& path, const std::string& command,
        std::size_t index, const ProgressEmitter& progress);
    void observe_result(const PermissionVerdict& verdict, const ToolCall& call,
        const std::string& path, const std::string& command, ToolResult& result);
private:
    ToolExecutor& tools_;
    PermissionManager& permissions_;
    WorkspaceBoundary& boundary_;
    SessionExecSecurity& security_;
    ToolSessionHost& host_;
    GoalRuntime& goal_;
    ToolHookBridge& tool_hooks_;
    ExecPermissionGate& exec_;
    PathAccessPolicy& paths_;
    PermissionConfirmation& confirmation_;
    SessionManager* session_manager_; // Nullable borrowed constructor dependency.
    HookManager* hook_manager_; // Nullable borrowed constructor dependency.
};
} // namespace acecode::agent
