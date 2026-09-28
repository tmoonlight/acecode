#pragma once
#include <array>
namespace acecode::tui {
enum class TuiShutdownStep {
    ModelPool, AutoTitle, Notifications, ActiveScreen, ConsoleHandler, StopAnimation, InboundSubmit, AbortAndWake, AgentWorker, Subagents, PowerLease, Mcp, Lsp, CompactWorker, AnimationWorker, AuthWorker, UpdateWorker, Worktree, FinalizeSession, CleanupSessions, SessionRegistration, ResumeHint, AbandonedWork
};
class ITuiShutdownActions {
public:
    virtual ~ITuiShutdownActions() = default;
    virtual void shutdown_step(TuiShutdownStep step) = 0;
    virtual void shutdown_error(TuiShutdownStep step) noexcept;
};
class TuiShutdownSequence {
public:
    void run(ITuiShutdownActions& actions) noexcept;
    static const std::array<TuiShutdownStep, 23> & order();
private:
    bool ran_ = false;
};
}
