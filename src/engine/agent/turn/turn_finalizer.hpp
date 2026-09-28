#pragma once
#include "tool/tool_executor.hpp"
#include "agent/callbacks_slot.hpp"
#include "turn_types.hpp"
#include "utils/lifetime_token.hpp"
#include <atomic>

namespace acecode {
class AbortSignal;
class SessionManager;
class EventDispatcher;
class ToolExecutor;
struct AgentCallbacks;
struct ToolCapabilityPolicy;
}
namespace acecode::agent {
class ConversationHistory;
class TranscriptWriter;
class TurnOutcomeRecord;
class ActiveTurnGate;
class GoalRuntime;
class AgentHookBridge;
class ActivityNarrator;
class TrajectoryRecorder;
struct TurnContext;

struct TurnFinalizerServices {
    ConversationHistory& history;
    TranscriptWriter& transcript;
    TurnOutcomeRecord& outcome;
    ActiveTurnGate& gate;
    GoalRuntime& goal;
    AgentHookBridge& hooks;
    ActivityNarrator& activity;
    EventDispatcher& events;
    CallbacksSlot& callbacks;
    ToolExecutor& tools;
    const ToolCapabilityPolicy& policy;
    std::atomic<bool>& busy;
    AbortSignal& abort_signal;
    std::atomic<bool>& turn_interrupt_requested;
    SessionManager* session_manager;
};

// One ordered step table preserves normal, prompt-hook and recovery differences.
// Recovery isolates only the reporting steps that were independently guarded.
class TurnFinalizer {
public:
    explicit TurnFinalizer(TurnFinalizerServices services);
    void normal(TurnContext& turn, int max_iterations, LifetimeRef<TrajectoryRecorder> trajectory);
    void hook_blocked(TurnContext& turn, const std::string& reason,
        LifetimeRef<TrajectoryRecorder> trajectory);
    void recover(TurnContext* turn, const char* detail, bool chat,
        LifetimeRef<TrajectoryRecorder> trajectory);
private:
    enum class Mode;
    enum class Step;
    struct Frame;
    void run(Frame& frame);
    void step(Step step, Frame& frame);
    void prepare_normal(Frame& frame);
    void after_normal(TurnContext& turn);
    void continue_goal();
    void message(const std::string& role, const std::string& content, bool is_tool,
        nlohmann::json metadata = nlohmann::json::object());
    ConversationHistory& history_;
    TranscriptWriter& transcript_;
    TurnOutcomeRecord& outcome_;
    ActiveTurnGate& gate_;
    GoalRuntime& goal_;
    AgentHookBridge& hooks_;
    ActivityNarrator& activity_;
    EventDispatcher& events_;
    CallbacksSlot& callbacks_;
    ToolExecutor& tools_;
    ToolCapabilityPolicy policy_;
    std::atomic<bool>& busy_;
    AbortSignal& abort_signal_;
    std::atomic<bool>& turn_interrupt_requested_;
    SessionManager* session_manager_; // Nullable borrowed constructor dependency.
};
} // namespace acecode::agent
