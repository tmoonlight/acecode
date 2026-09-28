#pragma once
#include "agent/callbacks_slot.hpp"
#include "tool/tool_executor.hpp"
#include "utils/lifetime_token.hpp"

namespace acecode { struct AgentCallbacks; }
namespace acecode::agent {
class WorkspaceBoundary;
class ToolSessionHost;
class TranscriptWriter;

class ToolResultPresenter {
public:
    ToolResultPresenter(WorkspaceBoundary& boundary, ToolSessionHost& host,
        TranscriptWriter& transcript, CallbacksSlot& callbacks, SessionManager* session)
        : boundary_(boundary), host_(host), transcript_(transcript),
          callbacks_(callbacks), session_manager_(session) {}
    void materialize_attachments(ToolResult& result);
    void display(const ToolCall& call, const ToolResult& result);
private:
    WorkspaceBoundary& boundary_;
    ToolSessionHost& host_;
    TranscriptWriter& transcript_;
    CallbacksSlot& callbacks_;
    SessionManager* session_manager_; // Nullable borrowed constructor dependency.
    LifetimeToken lifetime_;
};
} // namespace acecode::agent
