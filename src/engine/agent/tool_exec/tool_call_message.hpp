#pragma once
#include "agent/turn/turn_types.hpp"

namespace acecode { class SessionManager; class EventDispatcher; class HookManager; }
namespace acecode::agent {
class ConversationHistory;
class AgentHookBridge;
class ToolCallMessage {
public:
    ToolCallMessage(ConversationHistory& history, AgentHookBridge& hooks,
        EventDispatcher& events, SessionManager* session, HookManager* manager)
        : history_(history), hooks_(hooks), events_(events),
          session_manager_(session), hook_manager_(manager) {}
    ToolPreambleTitle record(const ChatResponse& response,
        const std::shared_ptr<LlmProvider>& provider, ToolPreambleTitle& pending_preamble);
private:
    ConversationHistory& history_;
    AgentHookBridge& hooks_;
    EventDispatcher& events_;
    SessionManager* session_manager_; // Nullable borrowed constructor dependency.
    HookManager* hook_manager_; // Nullable borrowed constructor dependency.
};
} // namespace acecode::agent
