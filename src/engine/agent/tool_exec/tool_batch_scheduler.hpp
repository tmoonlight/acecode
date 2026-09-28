#pragma once
#include "agent/callbacks_slot.hpp"

#include "ask_question_binding.hpp"
#include "tool_context_factory.hpp"
#include "tool_invoker.hpp"
#include "tool_result_presenter.hpp"
#include "tool_call_lifecycle.hpp"
#include "tool_call_message.hpp"
#include "tool_result_committer.hpp"
#include "agent/approval/tool_permission_gate.hpp"

namespace acecode::agent {

// Composition input only: no collaborator keeps this bag or calls AgentLoop.
struct ToolExecutionServices {
    ToolExecutor& tools;
    CallbacksSlot& callbacks;
    PermissionManager& permissions;
    ConversationHistory& history;
    TranscriptWriter& transcript;
    AgentHookBridge& hooks;
    ToolHookBridge& tool_hooks;
    WorkspaceBoundary& boundary;
    SessionExecSecurity& security;
    PromptContextCache& prompt_cache;
    GoalRuntime& goal;
    EventDispatcher& events;
    AbortSignal& abort;
    const AgentLoopConfig& config;
    const ToolCapabilityPolicy& policy;
    SessionManager* session; // Nullable borrowed constructor dependencies below.
    HookManager* hook_manager;
    const SkillRegistry* skills;
    PermissionPrompter* permission_prompter;
    AskUserQuestionPrompter* question_prompter;
};
struct ToolExecutionOptions {
    ToolContextFactory::ProviderAccessor provider;
    AskQuestionChannel question_channel;
    ToolLifecycleEvents::Clock clock;
    std::vector<std::string> model_tool_names;
};

// One joined batch owns this tool chain. The composition root owns the scope;
// contexts/prompters remain bound until every future and callback has drained.
class ToolBatchScheduler {
public:
    ToolBatchScheduler(ToolExecutionServices services, ToolExecutionOptions options);
    ToolBatchOutcome execute(const ChatResponse& response, const std::shared_ptr<LlmProvider>& provider,
        const ProgressEmitter& progress, SynchronizedDoomGuard& doom_guard,
        ToolPreambleTitle& pending_preamble);
private:
    ToolExecutor& tools_;
    TranscriptWriter& transcript_;
    GoalRuntime& goal_;
    AbortSignal& abort_signal_;
    SessionManager* session_manager_; // Nullable borrowed constructor dependency.
    // Dependencies precede users; destruction runs in the reverse order.
    ToolContextFactory contexts_;
    PathAccessPolicy paths_;
    ExecPermissionGate exec_;
    PermissionConfirmation confirmation_;
    ToolPermissionGate gate_;
    ToolInvoker invoker_;
    ToolResultPresenter presenter_;
    ToolLifecycleEvents lifecycle_events_;
    AskQuestionBinding questions_;
    ToolCallLifecycle lifecycle_;
    ToolCallMessage message_;
    ToolResultCommitter committer_;
};
} // namespace acecode::agent
