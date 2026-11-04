#pragma once
#include "agent/callbacks_slot.hpp"
#include "agent/agent_runtime_env.hpp"

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
struct CompactionInputs;

// 通用恢复链的「先压缩」(PA 兜底的同一步记在 pa::RescueState 里)。请求被收下
// 时清零,之后再被拒是新的一轮,重新先压缩。
struct OverflowCompactionEpisode {
    bool compaction_attempted = false;
    // 决定先压缩时被拒请求的规模;压缩后仍被拒时按它给学习器记账。
    int rejected_tokens = 0;
    bool rejection_recorded = false;
};

struct RequestRecoveryState {
    ContextRecoveryStage stage = ContextRecoveryStage::Normal;
    bool emergency_profile = false;
    pa::RescueState pa_episode;
    OverflowCompactionEpisode overflow_episode;
    bool skip_auto_compact_once = false;
    // 通用恢复链本轮是否已经撤过内联图片。请求被收下时清零,之后再带图被拒
    // (新的图片进来了)重新撤一次。
    bool inline_images_withheld = false;
};
struct RecoveryDecision {
    HandleErrorResult decision = HandleErrorResult::Proceed;
    std::optional<std::string> timing_status;
};

class ContextOverflowRecovery {
public:
    ContextOverflowRecovery(ConversationHistory& history, TranscriptWriter& transcript,
        CompactionController& compaction, RetryProgressReporter& retry, GoalRuntime& goal,
        CallbacksSlot& callbacks, EventDispatcher& events, AbortSignal& abort, AgentRuntimeEnv environment = {})
        : history_(history), transcript_(transcript), compaction_(compaction), retry_(retry),
          goal_(goal), callbacks_(callbacks), events_(events), abort_(abort), environment_(std::move(environment)) {}
    // compaction: 超限时「先压缩」用的入参,只在本次调用期间借用;为空则跳过
    // 压缩这一步(直接收缩并记账)。
    RecoveryDecision resolve(const ProviderCallResult& result,
        const std::vector<ChatMessage>& messages, RequestRecoveryState& state,
        int declared_window, SessionManager* session,
        const CompactionInputs* compaction = nullptr);
private:
    ConversationHistory& history_;
    TranscriptWriter& transcript_;
    CompactionController& compaction_;
    RetryProgressReporter& retry_;
    GoalRuntime& goal_;
    CallbacksSlot& callbacks_;
    EventDispatcher& events_;
    AbortSignal& abort_;
    AgentRuntimeEnv environment_;
};
} // namespace acecode::agent
