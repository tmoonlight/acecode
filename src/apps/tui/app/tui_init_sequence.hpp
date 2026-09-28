#pragma once
#include "tui/app/tui_shutdown_sequence.hpp"
#include <array>
namespace acecode::tui {
enum class TuiInitStage {
    Environment, Services, InitialState, Screen, UpdateCheck, AskAndMcp, CopilotAuth, TokenTracking, AgentAssembly, ModelPool, MainSession, AutoTitle, Subagents, ProcessRegistrations, ResumeStartup, Commands, Notifications, ResumePicker, FinalAgentCallbacks, InboundSubmit, Animation, Components
};
class ITuiApplicationLifecycle : public ITuiShutdownActions {
public:
    virtual bool init_stage(TuiInitStage stage) = 0;
    virtual void run_event_loop() = 0;
};
class TuiInitSequence {
public:
    bool run(ITuiApplicationLifecycle& app);
    static const std::array<TuiInitStage, 22> & order();
};
// Used by the real app and fault-injection fixtures: all early returns and
// exceptions take the same shutdown path while the owners are still alive.
int run_tui_application(ITuiApplicationLifecycle& app, TuiShutdownSequence& shutdown);
}
