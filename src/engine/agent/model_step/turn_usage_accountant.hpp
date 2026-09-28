#pragma once

#include "agent/turn/turn_types.hpp"
#include <atomic>

namespace acecode { struct AgentCallbacks; class EventDispatcher; class SessionManager; }
namespace acecode::agent {
class GoalRuntime;
struct TurnUsageRecord {
    TokenUsage aggregate;
    bool initialized = false;
};

// Record first, publish second: worker recovery still sees charged usage when
// a callback, session writer or event consumer throws.
class TurnUsageAccountant {
public:
    TurnUsageAccountant(GoalRuntime& goal, AgentCallbacks& callbacks,
                        EventDispatcher& events, std::atomic<int>& context_tokens)
        : goal_(goal), callbacks_(callbacks), events_(events), context_tokens_(context_tokens) {}
    void accept(TurnUsageRecord& record, const TokenUsage& usage, SessionManager* session);
    TokenUsage estimate(TurnUsageRecord& record, const ChatResponse& response,
                        const ApiRequestBundle& bundle, SessionManager* session);
private:
    GoalRuntime& goal_;
    AgentCallbacks& callbacks_;
    EventDispatcher& events_;
    std::atomic<int>& context_tokens_;
};
} // namespace acecode::agent
