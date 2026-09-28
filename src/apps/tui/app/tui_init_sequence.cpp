#include "tui/app/tui_init_sequence.hpp"
#include "utils/scope_exit.hpp"
namespace acecode::tui {
const std::array<TuiInitStage, 22> & TuiInitSequence::order() {
    static const std::array<TuiInitStage, 22> stages{{
        TuiInitStage::Environment,
        TuiInitStage::Services,
        TuiInitStage::InitialState,
        TuiInitStage::Screen,
        TuiInitStage::UpdateCheck,
        TuiInitStage::AskAndMcp,
        TuiInitStage::CopilotAuth,
        TuiInitStage::TokenTracking,
        TuiInitStage::AgentAssembly,
        TuiInitStage::ModelPool,
        TuiInitStage::MainSession,
        TuiInitStage::AutoTitle,
        TuiInitStage::Subagents,
        TuiInitStage::ProcessRegistrations,
        TuiInitStage::ResumeStartup,
        TuiInitStage::Commands,
        TuiInitStage::Notifications,
        TuiInitStage::ResumePicker,
        TuiInitStage::FinalAgentCallbacks,
        TuiInitStage::InboundSubmit,
        TuiInitStage::Animation,
        TuiInitStage::Components,
    }};
    return stages;
}
bool TuiInitSequence::run(ITuiApplicationLifecycle& app) {
    for (auto stage : order()) if (!app.init_stage(stage)) return false;
    return true;
}
int run_tui_application(ITuiApplicationLifecycle& app, TuiShutdownSequence& shutdown) {
    ScopeExit finish([&] { shutdown.run(app); });
    if (!TuiInitSequence{}.run(app)) return 1;
    app.run_event_loop();
    return 0;
}
}
