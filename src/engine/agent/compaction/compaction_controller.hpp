#pragma once
#include "agent/callbacks_slot.hpp"
#include "agent/agent_runtime_env.hpp"

#include "agent/request/api_request_builder.hpp"
#include "utils/lifetime_token.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace acecode {
struct AgentCallbacks;
class EventDispatcher;
class AbortSignal;
class SessionManager;
class HookManager;
struct CompactResult;
}
namespace acecode::agent {
class ConversationHistory;
class TranscriptWriter;
class TrajectoryRecorder;
class WorkspaceBoundary;
class AgentHookBridge;
class ActiveProviderSlot;
class RetryProgressReporter;

// Per-call inputs; session/hooks are nullable borrows and never retained.
struct CompactionInputs {
    SessionManager* session = nullptr;
    HookManager* hooks = nullptr;
    std::shared_ptr<LlmProvider> provider; // shared with the active provider scope
    RequestContextOptions request;
    int suggestion_threshold = 3;
    LifetimeRef<TrajectoryRecorder> terminal;
};

class CompactionController {
public:
    CompactionController(ConversationHistory& history, TranscriptWriter& transcript,
        WorkspaceBoundary& boundary, AgentHookBridge& hooks, ApiRequestBuilder& requests,
        ActiveProviderSlot& provider, RetryProgressReporter& retry, CallbacksSlot& callbacks,
        EventDispatcher& events, AbortSignal& abort, std::atomic<bool>& busy,
        std::atomic<int>& context_tokens, AgentRuntimeEnv environment = {})
        : history_(history), transcript_(transcript), boundary_(boundary), hooks_(hooks),
          requests_(requests), active_provider_(provider), retry_(retry), callbacks_(callbacks),
          events_(events), abort_(abort), busy_(busy), last_api_total_tokens_(context_tokens), environment_(std::move(environment)) {}
    bool run_auto(const CompactionInputs& inputs);
    void run_manual(const CompactionInputs& inputs);
    bool exceeds_auto_threshold(const CompactionInputs& inputs, const UserInput* pending) const;
    void mark_history_repaired();
    int generation() const { return compact_generation_.load(std::memory_order_relaxed); }
    void reset_window();
private:
    bool mechanical_fallback(const CompactionInputs& inputs, int request_tokens,
        int context_window, const std::string& notice_id, const std::string& error);
    void initialize_window(const CompactionInputs& inputs);
    void apply_result(const CompactionInputs& inputs, const CompactResult& result,
                      const std::string& trigger, const std::string& notice_id);
    void finish_busy(LifetimeRef<TrajectoryRecorder> terminal);

    ConversationHistory& history_;
    TranscriptWriter& transcript_;
    WorkspaceBoundary& boundary_;
    AgentHookBridge& hooks_;
    ApiRequestBuilder& requests_;
    ActiveProviderSlot& active_provider_;
    RetryProgressReporter& retry_;
    CallbacksSlot& callbacks_;
    EventDispatcher& events_;
    AbortSignal& abort_;
    std::atomic<bool>& busy_;
    std::atomic<int>& last_api_total_tokens_;
    AgentRuntimeEnv environment_;
    std::atomic<int> compact_generation_{0};
    bool compact_window_initialized_ = false;
    std::uint64_t compact_window_number_ = 0;
    std::string compact_first_window_id_;
    std::string compact_current_window_id_;
    LifetimeToken lifetime_;
};
} // namespace acecode::agent
