#include <gtest/gtest.h>
#include "daemon/daemon_shutdown_sequence.hpp"
#include "utils/scope_exit.hpp"
#include <map>
#include <stdexcept>

namespace {
using acecode::daemon::DaemonShutdownSequence;
using acecode::daemon::DaemonShutdownStep;

TEST(DaemonShutdownSequence, StopsProducersAndSessionsBeforeTheirServices) {
    bool scheduler_running = true;
    bool suggestions_running = true;
    bool sessions_running = true;
    bool mcp_available = true;
    bool lsp_available = true;
    bool runtime_files_exist = true;
    std::map<DaemonShutdownStep, int> calls;
    DaemonShutdownSequence shutdown;
    const auto action = [&](DaemonShutdownStep step) {
        ++calls[step];
        switch (step) {
        case DaemonShutdownStep::LoopScheduler: scheduler_running = false; break;
        case DaemonShutdownStep::TaskSuggestions: suggestions_running = false; break;
        case DaemonShutdownStep::Sessions:
            EXPECT_FALSE(scheduler_running);
            EXPECT_FALSE(suggestions_running);
            EXPECT_TRUE(mcp_available);
            EXPECT_TRUE(lsp_available);
            sessions_running = false;
            break;
        case DaemonShutdownStep::SpawnListener: EXPECT_FALSE(sessions_running); break;
        case DaemonShutdownStep::Mcp:
            EXPECT_FALSE(sessions_running);
            mcp_available = false;
            break;
        case DaemonShutdownStep::Lsp:
            EXPECT_FALSE(sessions_running);
            lsp_available = false;
            break;
        case DaemonShutdownStep::RuntimeFiles:
            EXPECT_FALSE(mcp_available);
            EXPECT_FALSE(lsp_available);
            runtime_files_exist = false;
            break;
        default: break;
        }
    };
    shutdown.run(action);
    shutdown.run(action);
    EXPECT_FALSE(runtime_files_exist);
    for (const auto& [step, count] : calls) { (void)step; EXPECT_EQ(count, 1); }
}

TEST(DaemonShutdownSequence, ExceptionStillRunsLaterCleanupOnce) {
    int registry_shutdowns = 0;
    int files_cleaned = 0;
    DaemonShutdownSequence shutdown;
    const auto action = [&](DaemonShutdownStep step) {
        if (step == DaemonShutdownStep::Channels) throw std::runtime_error("injected stop fault");
        if (step == DaemonShutdownStep::Sessions) ++registry_shutdowns;
        if (step == DaemonShutdownStep::RuntimeFiles) ++files_cleaned;
    };
    try {
        acecode::ScopeExit cleanup([&] { shutdown.run(action); });
        throw std::runtime_error("injected server failure");
    } catch (const std::runtime_error&) {}
    shutdown.run(action);
    EXPECT_EQ(registry_shutdowns, 1);
    EXPECT_EQ(files_cleaned, 1);
}
} // namespace
