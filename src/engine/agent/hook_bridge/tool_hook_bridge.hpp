#pragma once

#include "hooks/hook_runtime.hpp"
#include "tool/tool_executor.hpp"

#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace acecode { class HookManager; class SessionManager; }

namespace acecode::agent {
class AgentHookBridge;

class ToolHookBridge {
public:
    explicit ToolHookBridge(AgentHookBridge& hooks) : hooks_(hooks) {}
    std::optional<ToolResult> before(HookManager* manager, SessionManager* session,
                                     ToolCall& call, std::size_t tool_index);
    void after(HookManager* manager, SessionManager* session, const ToolCall& call, ToolResult& result);
    HookAggregateOutcome permission_request(HookManager* manager, SessionManager* session,
                                             const std::string& tool, const nlohmann::json& input);
    void permission_resolved(HookManager* manager, SessionManager* session,
                              const std::string& tool, const nlohmann::json& input,
                              const std::string& decision, const std::string& source);
private:
    AgentHookBridge& hooks_;
};

// Function-local permission transaction. Constructor dependencies are borrowed
// and fixed for this synchronous tool call; this object cannot be copied or
// retained by an asynchronous callback. Resolution disarms before dispatch.
// Unwinding emits no new hook events, matching the previous exception path.
class PermissionHookSession {
public:
    PermissionHookSession(ToolHookBridge& bridge, HookManager* manager, SessionManager* session,
                          std::string tool)
        : bridge_(bridge), manager_(manager), session_(session), tool_(std::move(tool)),
          exceptions_(std::uncaught_exceptions()) {}
    ~PermissionHookSession() noexcept(false);
    PermissionHookSession(const PermissionHookSession&) = delete;
    PermissionHookSession& operator=(const PermissionHookSession&) = delete;
    HookAggregateOutcome request(nlohmann::json input);
    void resolve(const std::string& decision, const std::string& source);
    bool pending() const { return requested_ && !resolved_; }
    // The legacy exec-without-channel denial emits Request without Resolved.
    void leave_unresolved() { auto_resolve_ = false; }
private:
    ToolHookBridge& bridge_;
    HookManager* manager_; // Nullable borrowed constructor dependency.
    SessionManager* session_; // Nullable borrowed constructor dependency.
    std::string tool_;
    nlohmann::json input_ = nlohmann::json::object();
    int exceptions_;
    bool auto_resolve_ = true;
    bool requested_ = false;
    bool resolved_ = false;
};

} // namespace acecode::agent
