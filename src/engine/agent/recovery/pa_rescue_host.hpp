#pragma once
#include "agent/callbacks_slot.hpp"

#include "pa/pa_rescue_driver.hpp"
#include "agent/model_step/active_model_view.hpp"
#include <utility>

namespace acecode { struct AgentCallbacks; class EventDispatcher; class AbortSignal; }
namespace acecode::agent {
class ConversationHistory;
class TranscriptWriter;
class CompactionController;
class RetryProgressReporter;

// Invocation-scoped adapter, never implemented by or pointed back at AgentLoop.
// Its fixed nullable session borrow cannot escape the synchronous driver call.
class PaRescueAdapter final : public pa::PaRescueHost {
public:
    PaRescueAdapter(ConversationHistory& history, TranscriptWriter& transcript,
        CompactionController& compaction, RetryProgressReporter& retry,
        CallbacksSlot& callbacks, EventDispatcher& events, AbortSignal& abort,
        SessionManager* session, ActiveModelView model)
        : history_(history), transcript_(transcript), compaction_(compaction), retry_(retry),
          callbacks_(callbacks), events_(events), abort_(abort), session_(session),
          model_(std::move(model)) {}
    int history_tokens() const override;
    void note_rejection(int request_tokens) override;
    ThreadRepairResult repair(const ThreadRepairOptions& options) override;
    bool wait(int milliseconds) override;
    void reset_stream() override;
    void history_repaired() override;
    void notice(const std::string& text, nlohmann::json metadata) override;
    void progress(nlohmann::json payload) override;
    void retry(const ProviderErrorInfo& info, bool waiting,
               std::string label, std::string detail) override;
private:
    ConversationHistory& history_;
    TranscriptWriter& transcript_;
    CompactionController& compaction_;
    RetryProgressReporter& retry_;
    CallbacksSlot& callbacks_;
    EventDispatcher& events_;
    AbortSignal& abort_;
    SessionManager* const session_; // nullable, borrowed until run_rescue returns
    ActiveModelView model_;
};
} // namespace acecode::agent
