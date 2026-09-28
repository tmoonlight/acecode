#pragma once

#include "agent/turn/turn_types.hpp"

namespace acecode {
class ToolExecutor;
struct AgentCallbacks;
class EventDispatcher;
class AbortSignal;
class SessionManager;
}
namespace acecode::agent {
class ConversationHistory;
class ActiveProviderSlot;
class ActivityNarrator;
class RetryProgressReporter;
class TurnUsageAccountant;
struct TurnUsageRecord;
class ModelStepRecorder;

class ProviderStreamCollector {
public:
    ProviderStreamCollector(ToolExecutor& tools, AgentCallbacks& callbacks,
        EventDispatcher& events, ConversationHistory& history, ActiveProviderSlot& provider,
        AbortSignal& abort, ActivityNarrator& activity, RetryProgressReporter& retry,
        TurnUsageAccountant& usage, ModelStepRecorder& recorder)
        : tools_(tools), callbacks_(callbacks), events_(events), history_(history),
          active_provider_(provider), abort_(abort), activity_(activity), retry_(retry),
          usage_(usage), recorder_(recorder) {}
    ProviderCallResult collect(const std::shared_ptr<LlmProvider>& provider,
        const ApiRequestBundle& bundle, const ProgressEmitter& emit_progress,
        int model_step_index, TurnUsageRecord& usage, SessionManager* session);
private:
    struct Call;
    ToolExecutor& tools_;
    AgentCallbacks& callbacks_;
    EventDispatcher& events_;
    ConversationHistory& history_;
    ActiveProviderSlot& active_provider_;
    AbortSignal& abort_;
    ActivityNarrator& activity_;
    RetryProgressReporter& retry_;
    TurnUsageAccountant& usage_;
    ModelStepRecorder& recorder_;
};
} // namespace acecode::agent
