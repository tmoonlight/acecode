#pragma once

#include "agent/turn/turn_types.hpp"
#include "pa/pa_overflow_rescue.hpp"
#include <optional>

namespace acecode { class SessionManager; class EventDispatcher; class AbortSignal; struct AgentCallbacks; }
namespace acecode::agent {
class ConversationHistory;
class TranscriptWriter;
class CompactionController;
class RetryProgressReporter;
class GoalRuntime;

struct RequestRecoveryState {
    ContextRecoveryStage stage = ContextRecoveryStage::Normal;
    bool emergency_profile = false;
    pa::RescueState pa_episode;
    bool skip_auto_compact_once = false;
};
struct RecoveryDecision {
    HandleErrorResult decision = HandleErrorResult::Proceed;
    std::optional<std::string> timing_status;
};

class ContextOverflowRecovery {
public:
    ContextOverflowRecovery(ConversationHistory& history, TranscriptWriter& transcript,
        CompactionController& compaction, RetryProgressReporter& retry, GoalRuntime& goal,
        AgentCallbacks& callbacks, EventDispatcher& events, AbortSignal& abort)
        : history_(history), transcript_(transcript), compaction_(compaction), retry_(retry),
          goal_(goal), callbacks_(callbacks), events_(events), abort_(abort) {}
    RecoveryDecision resolve(const ProviderCallResult& result,
        const std::vector<ChatMessage>& messages, RequestRecoveryState& state,
        int declared_window, SessionManager* session);
private:
    ConversationHistory& history_;
    TranscriptWriter& transcript_;
    CompactionController& compaction_;
    RetryProgressReporter& retry_;
    GoalRuntime& goal_;
    AgentCallbacks& callbacks_;
    EventDispatcher& events_;
    AbortSignal& abort_;
};
} // namespace acecode::agent
