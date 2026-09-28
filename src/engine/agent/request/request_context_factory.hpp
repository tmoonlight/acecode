#pragma once
#include "api_request_builder.hpp"
#include <atomic>

namespace acecode { class PermissionManager; }
namespace acecode::agent {
class WorkspaceBoundary;
class SessionExecSecurity;
class AgentHookBridge;
class ConversationHistory;
struct RequestContextSource;

// Reads cwd/worktree/sandbox/model at request construction time; it never calls
// back into the facade. Constructor references outlive this synchronous scope.
class RequestContextFactory {
public:
    RequestContextFactory(WorkspaceBoundary& boundary, SessionExecSecurity& security,
        const RequestContextSource& source, const std::atomic<int>& context_window,
        SessionManager* session, ToolExecutor& tools, PermissionManager& permissions,
        ConversationHistory& history, ApiRequestBuilder& builder, AgentHookBridge& hooks)
        : boundary_(boundary), security_(security), source_(source), context_window_(context_window),
          session_manager_(session), tools_(tools), permissions_(permissions), history_(history),
          builder_(builder), hooks_(hooks) {}
    RequestContextOptions options(const std::shared_ptr<LlmProvider>& provider,
        bool swarm_mode = false) const;
    ApiRequestBundle build(const std::shared_ptr<LlmProvider>& provider,
        bool emergency_profile = false, bool swarm_mode = false);
private:
    WorkspaceBoundary& boundary_;
    SessionExecSecurity& security_;
    const RequestContextSource& source_;
    const std::atomic<int>& context_window_;
    SessionManager* session_manager_; // Nullable borrowed constructor dependency.
    ToolExecutor& tools_;
    PermissionManager& permissions_;
    ConversationHistory& history_;
    ApiRequestBuilder& builder_;
    AgentHookBridge& hooks_;
};
} // namespace acecode::agent
