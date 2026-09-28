#pragma once
#include "agent/callbacks_slot.hpp"
#include "agent/request/request_context_factory.hpp"
#include "agent/tool_exec/tool_batch_scheduler.hpp"
#include "turn_finalizer.hpp"

namespace acecode::agent {
class CompactionController;
struct CompactionInputs;
class ContextOverflowRecovery;
class ProviderStreamCollector;
class SideQuestionService;
class TurnUsageAccountant;
class ModelStepRecorder;

// Constructor input only; the runner keeps fixed, typed collaborator references.
struct TurnRunnerServices {
    ToolExecutionServices tools;
    TurnOutcomeRecord& outcome;
    ActiveTurnGate& gate;
    ActivityNarrator& activity;
    SideQuestionService& side_questions;
    TurnUsageAccountant& usage;
    ModelStepRecorder& steps;
    ProviderStreamCollector& stream;
    CompactionController& compaction;
    ContextOverflowRecovery& recovery;
    ApiRequestBuilder& requests;
    const RequestContextSource& request_source;
    std::atomic<bool>& busy;
    std::atomic<bool>& interrupt;
    const std::atomic<int>& context_window;
    const std::atomic<int>& suggestion_threshold;
};
struct TurnRunnerOptions {
    ToolContextFactory::ProviderAccessor provider;
    AskQuestionChannel question_channel;
    ToolLifecycleEvents::Clock clock;
    std::function<void(const std::string&)> computer_use_release;
    std::string no_model_prompt;
};

class TurnRunner {
public:
    TurnRunner(TurnRunnerServices services, TurnRunnerOptions options);
    void run(TurnContext& turn, const UserInput& input, bool hidden_goal_context,
        const ChatMessage* retry_message, LifetimeRef<TrajectoryRecorder> terminal);
private:
    CompactionInputs compaction_inputs(const TurnContext& turn,
        LifetimeRef<TrajectoryRecorder> terminal) const;
    bool drain_inputs(bool close_if_empty);
    ToolBatchOutcome execute_tools(TurnContext& turn, const ChatResponse& response,
        const std::shared_ptr<LlmProvider>& provider, const ProgressEmitter& progress);

    ToolExecutor& tools_;
    CallbacksSlot& callbacks_;
    PermissionManager& permissions_;
    ConversationHistory& history_;
    TranscriptWriter& transcript_;
    AgentHookBridge& hooks_;
    ToolHookBridge& tool_hooks_;
    WorkspaceBoundary& boundary_;
    SessionExecSecurity& security_;
    PromptContextCache& prompt_cache_;
    GoalRuntime& goal_;
    EventDispatcher& events_;
    AbortSignal& abort_;
    const AgentLoopConfig& config_;
    const RequestContextSource& source_;
    SessionManager* session_; // Nullable borrowed constructor dependencies below.
    HookManager* hook_manager_;
    PermissionPrompter* permission_prompter_;
    AskUserQuestionPrompter* question_prompter_;
    TurnOutcomeRecord& outcome_;
    ActiveTurnGate& gate_;
    ActivityNarrator& activity_;
    SideQuestionService& side_questions_;
    TurnUsageAccountant& usage_;
    ModelStepRecorder& steps_;
    ProviderStreamCollector& stream_;
    CompactionController& compaction_;
    ContextOverflowRecovery& recovery_;
    std::atomic<bool>& busy_;
    std::atomic<bool>& interrupt_;
    const std::atomic<int>& context_window_;
    const std::atomic<int>& suggestion_threshold_;
    TurnRunnerOptions options_;
    RequestContextFactory requests_;
    TurnFinalizer finalizer_;
};
} // namespace acecode::agent
