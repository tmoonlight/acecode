#pragma once
#include "agent/callbacks_slot.hpp"
#include "tool_batch_types.hpp"
#include "tool_lifecycle_events.hpp"
#include "utils/lifetime_token.hpp"

namespace acecode { class HookManager; }
namespace acecode::agent {
class ToolHookBridge;
class ToolContextFactory;
class AskQuestionBinding;
class ToolInvoker;
class ToolResultPresenter;

class ToolCallLifecycle {
public:
    ToolCallLifecycle(ToolHookBridge& hooks, ToolContextFactory& contexts,
        AskQuestionBinding& questions, ToolInvoker& invoker, ToolResultPresenter& presenter,
        ToolLifecycleEvents& lifecycle_events, EventDispatcher& events, CallbacksSlot& callbacks,
        HookManager* manager, SessionManager* session, ToolLifecycleEvents::Clock clock)
        : tool_hooks_(hooks), contexts_(contexts), questions_(questions), invoker_(invoker),
          presenter_(presenter), lifecycle_events_(lifecycle_events), events_(events),
          callbacks_(callbacks), hook_manager_(manager), session_manager_(session),
          stream_clock_(std::move(clock)) {}
    ToolCallOutcome run(ToolBatchState& batch, ToolCall call, std::size_t index, bool emit_tui);
    LifetimeRef<ToolCallLifecycle> ref() { return lifetime_.ref(*this); }
private:
    ToolHookBridge& tool_hooks_;
    ToolContextFactory& contexts_;
    AskQuestionBinding& questions_;
    ToolInvoker& invoker_;
    ToolResultPresenter& presenter_;
    ToolLifecycleEvents& lifecycle_events_;
    EventDispatcher& events_;
    CallbacksSlot& callbacks_;
    HookManager* hook_manager_; // Nullable borrowed constructor dependency.
    SessionManager* session_manager_; // Nullable borrowed constructor dependency.
    ToolLifecycleEvents::Clock stream_clock_;
    LifetimeToken lifetime_;
};
} // namespace acecode::agent
