#include "tui/app/tui_shutdown_sequence.hpp"
#include <cstdio>
namespace acecode::tui {
void ITuiShutdownActions::shutdown_error(TuiShutdownStep step) noexcept {
    std::fprintf(stderr, "[tui] shutdown step %d failed\n", static_cast<int>(step));
}
const std::array<TuiShutdownStep, 23> & TuiShutdownSequence::order() {
    static const std::array<TuiShutdownStep, 23> steps{{
        TuiShutdownStep::ModelPool,
        TuiShutdownStep::AutoTitle,
        TuiShutdownStep::Notifications,
        TuiShutdownStep::ActiveScreen,
        TuiShutdownStep::ConsoleHandler,
        TuiShutdownStep::StopAnimation,
        TuiShutdownStep::InboundSubmit,
        TuiShutdownStep::AbortAndWake,
        TuiShutdownStep::AgentWorker,
        TuiShutdownStep::Subagents,
        TuiShutdownStep::PowerLease,
        TuiShutdownStep::Mcp,
        TuiShutdownStep::Lsp,
        TuiShutdownStep::CompactWorker,
        TuiShutdownStep::AnimationWorker,
        TuiShutdownStep::AuthWorker,
        TuiShutdownStep::UpdateWorker,
        TuiShutdownStep::Worktree,
        TuiShutdownStep::FinalizeSession,
        TuiShutdownStep::CleanupSessions,
        TuiShutdownStep::SessionRegistration,
        TuiShutdownStep::ResumeHint,
        TuiShutdownStep::AbandonedWork,
    }};
    return steps;
}
void TuiShutdownSequence::run(ITuiShutdownActions& actions) noexcept {
    if (ran_) return;
    ran_ = true;
    for (auto step : order()) {
        try { actions.shutdown_step(step); }
        catch (...) { actions.shutdown_error(step); }
    }
}
}
