#pragma once
#include "agent/callbacks_slot.hpp"
#include "turn_types.hpp"

namespace acecode {
class SessionManager;
class SkillRegistry;
class SkillUsageStore;
class EventDispatcher;
struct AgentCallbacks;
}
namespace acecode::agent {
class ConversationHistory;
class TranscriptWriter;
class ActiveTurnGate;

// All dependencies are bound before publishing the user turn. Construction
// borrows may be null; a retry reuses identity/checkpoint exactly as before.
class TurnLifecycle {
public:
    TurnLifecycle(ConversationHistory& history, TranscriptWriter& transcript,
        ActiveTurnGate& gate, EventDispatcher& events, CallbacksSlot& callbacks,
        SessionManager* session, const SkillRegistry* skills, SkillUsageStore* usage)
        : history_(history), transcript_(transcript), gate_(gate), events_(events),
          callbacks_(callbacks), session_manager_(session), skill_registry_(skills),
          skill_usage_store_(usage) {}
    UserTurnInfo prepare_user_turn(const UserInput& input, bool hidden_goal_context);
    UserTurnInfo prepare_retry_user_turn(const ChatMessage& message);
private:
    void append_user_turn_message(UserTurnInfo& info, bool hidden_goal_context);
    void start_user_turn(const UserTurnInfo& info);
    ConversationHistory& history_;
    TranscriptWriter& transcript_;
    ActiveTurnGate& gate_;
    EventDispatcher& events_;
    CallbacksSlot& callbacks_;
    SessionManager* session_manager_; // Nullable borrowed constructor dependency.
    const SkillRegistry* skill_registry_; // Nullable borrowed constructor dependency.
    SkillUsageStore* skill_usage_store_; // Nullable borrowed constructor dependency.
};
} // namespace acecode::agent
