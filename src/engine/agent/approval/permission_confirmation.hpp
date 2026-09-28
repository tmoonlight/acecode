#pragma once
#include "tool/tool_executor.hpp"
#include "sandbox/exec_permission.hpp"
#include "agent/turn/turn_types.hpp"

namespace acecode {
class PermissionManager;
class PermissionPrompter;
class AbortSignal;
struct AgentCallbacks;
}
namespace acecode::agent {
class SessionExecSecurity;
class ToolSessionHost;
class PermissionAuditScope;
class PermissionHookSession;

class PermissionConfirmation {
public:
    PermissionConfirmation(PermissionManager& permissions, SessionExecSecurity& security,
        ToolSessionHost& host, AgentCallbacks& callbacks, AbortSignal& abort,
        SessionManager* session, PermissionPrompter* prompter)
        : permissions_(permissions), security_(security), host_(host), callbacks_(callbacks),
          abort_signal_(abort), session_manager_(session), prompter_(prompter) {}
    bool available() const;
    std::optional<ToolResult> confirm(const ToolCall& call, const std::string& command,
        std::size_t index, const ProgressEmitter& progress,
        std::optional<sandbox::ExecPermission>& exec, ToolContext& context,
        PermissionAuditScope& audit, PermissionHookSession& hooks);
private:
    PermissionManager& permissions_;
    SessionExecSecurity& security_;
    ToolSessionHost& host_;
    AgentCallbacks& callbacks_;
    AbortSignal& abort_signal_;
    SessionManager* session_manager_; // Nullable borrowed constructor dependency.
    PermissionPrompter* prompter_; // Nullable borrowed constructor dependency.
};
} // namespace acecode::agent
