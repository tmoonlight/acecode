#pragma once
#include "tool/tool_executor.hpp"
#include "sandbox/exec_permission.hpp"

namespace acecode { class PermissionManager; }
namespace acecode::agent {
class SessionExecSecurity;
class WorkspaceBoundary;
class ToolSessionHost;
class GoalRuntime;

class ExecPermissionGate {
public:
    ExecPermissionGate(SessionExecSecurity& security, PermissionManager& permissions,
        WorkspaceBoundary& boundary, ToolSessionHost& host, GoalRuntime& goal,
        SessionManager* session)
        : security_(security), permissions_(permissions), boundary_(boundary),
          host_(host), goal_(goal), session_manager_(session) {}
    std::optional<ToolResult> prepare(const ToolCall& call, const std::string& command,
        ToolContext& context, std::optional<sandbox::ExecPermission>& permission);
    void observe(const std::optional<sandbox::ExecPermission>& permission,
        const std::string& audit_sandbox, const std::string& command, ToolResult& result);
private:
    SessionExecSecurity& security_;
    PermissionManager& permissions_;
    WorkspaceBoundary& boundary_;
    ToolSessionHost& host_;
    GoalRuntime& goal_;
    SessionManager* session_manager_; // Nullable borrowed constructor dependency.
};
} // namespace acecode::agent
