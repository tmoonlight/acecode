#pragma once
#include "agent/callbacks_slot.hpp"

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
class ModelStepSink;

class ProviderStreamCollector {
public:
    ProviderStreamCollector(ToolExecutor& tools, CallbacksSlot& callbacks,
        EventDispatcher& events, ConversationHistory& history, ActiveProviderSlot& provider,
        AbortSignal& abort, ActivityNarrator& activity, RetryProgressReporter& retry)
        : tools_(tools), callbacks_(callbacks), events_(events), history_(history),
          active_provider_(provider), abort_(abort), activity_(activity), retry_(retry) {}
    ProviderCallResult collect(const std::shared_ptr<LlmProvider>& provider,
        const ApiRequestBundle& bundle, const ProgressEmitter& emit_progress,
        int model_step_index, ModelStepSink& sink, SessionManager* session);
private:
    struct Call;
    ToolExecutor& tools_;
    CallbacksSlot& callbacks_;
    EventDispatcher& events_;
    ConversationHistory& history_;
    ActiveProviderSlot& active_provider_;
    AbortSignal& abort_;
    ActivityNarrator& activity_;
    RetryProgressReporter& retry_;
};
} // namespace acecode::agent
