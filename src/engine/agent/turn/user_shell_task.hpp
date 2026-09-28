#pragma once
#include "utils/lifetime_token.hpp"
#include <atomic>
#include <string>

namespace acecode {
class ToolExecutor;
class AbortSignal;
class EventDispatcher;
class SessionManager;
class HookManager;
struct AgentCallbacks;
}
namespace acecode::agent {
class WorkspaceBoundary;
class TranscriptWriter;
class TrajectoryRecorder;
class AgentHookBridge;

// User-authored shell keeps its own smaller ToolContext and busy cycle;
// no BusyChanged(true) frame and no on_turn_finished callback are synthesized.
class UserShellTask {
public:
    UserShellTask(ToolExecutor& tools, WorkspaceBoundary& boundary, AbortSignal& abort,
        std::atomic<bool>& busy, AgentCallbacks& callbacks, EventDispatcher& events,
        TranscriptWriter& transcript, AgentHookBridge& hooks)
        : tools_(tools), boundary_(boundary), abort_signal_(abort), busy_(busy),
          callbacks_(callbacks), events_(events), transcript_(transcript), hooks_(hooks) {}
    void run(std::string command, SessionManager* session, HookManager* hooks,
        LifetimeRef<TrajectoryRecorder> terminal);
private:
    void finish_busy(LifetimeRef<TrajectoryRecorder> terminal);
    ToolExecutor& tools_;
    WorkspaceBoundary& boundary_;
    AbortSignal& abort_signal_;
    std::atomic<bool>& busy_;
    AgentCallbacks& callbacks_;
    EventDispatcher& events_;
    TranscriptWriter& transcript_;
    AgentHookBridge& hooks_;
    LifetimeToken lifetime_;
};
} // namespace acecode::agent
