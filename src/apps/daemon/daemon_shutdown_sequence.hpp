#pragma once
#include <array>
#include <functional>

namespace acecode::daemon {
enum class DaemonShutdownStep {
    Channels, RemoteWeb, RemoteControl, ConnectorWorkers, Watchers, LoopScheduler,
    TaskSuggestions, Sessions, SpawnListener, Mcp, Lsp, ModelPool, Heartbeat, RuntimeFiles
};
class DaemonShutdownSequence {
public:
    using Action = std::function<void(DaemonShutdownStep)>;
    void run(const Action& action) noexcept;
    static const std::array<DaemonShutdownStep, 14>& order();
private:
    bool ran_ = false;
};
} // namespace acecode::daemon
