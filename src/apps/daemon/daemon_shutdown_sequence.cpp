#include "daemon_shutdown_sequence.hpp"
#include <cstdio>
namespace acecode::daemon {
const std::array<DaemonShutdownStep, 15>& DaemonShutdownSequence::order() {
    static const std::array<DaemonShutdownStep, 15> steps{{
        DaemonShutdownStep::Channels, DaemonShutdownStep::ImChannels, DaemonShutdownStep::RemoteWeb,
        DaemonShutdownStep::RemoteControl, DaemonShutdownStep::ConnectorWorkers,
        DaemonShutdownStep::Watchers, DaemonShutdownStep::LoopScheduler,
        DaemonShutdownStep::TaskSuggestions, DaemonShutdownStep::Sessions,
        DaemonShutdownStep::SpawnListener, DaemonShutdownStep::Mcp,
        DaemonShutdownStep::Lsp, DaemonShutdownStep::ModelPool,
        DaemonShutdownStep::Heartbeat, DaemonShutdownStep::RuntimeFiles
    }};
    return steps;
}
void DaemonShutdownSequence::run(const Action& action) noexcept {
    if (ran_) return;
    ran_ = true;
    for (const auto step : order()) {
        try { action(step); }
        catch (...) {
            std::fprintf(stderr, "[daemon] shutdown step %d failed\n", static_cast<int>(step));
        }
    }
}
} // namespace acecode::daemon
