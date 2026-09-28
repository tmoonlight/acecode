#include <gtest/gtest.h>
#include "tui/app/tui_init_sequence.hpp"
#include "utils/joining_thread.hpp"
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <vector>
namespace {
using acecode::tui::TuiInitStage;
using acecode::tui::TuiShutdownStep;
struct WorkerProbe {
    std::mutex mutex;
    std::condition_variable changed;
    bool stopped = false;
    bool timed_out = false;
};
class LifecycleProbe final : public acecode::tui::ITuiApplicationLifecycle {
public:
    std::optional<TuiInitStage> throw_after, fail_at;
    bool throw_in_loop = false, entered_loop = false;
    std::optional<TuiShutdownStep> throw_on_shutdown;
    std::vector<TuiInitStage> initialized;
    std::vector<TuiShutdownStep> shutdowns, errors;
    std::shared_ptr<WorkerProbe> state = std::make_shared<WorkerProbe>();
    std::optional<acecode::JoiningThread> worker;
    ~LifecycleProbe() {
        stop_worker();
        if (worker) worker->join();
    }
    bool init_stage(TuiInitStage stage) override {
        initialized.push_back(stage);
        if (stage == TuiInitStage::AgentAssembly) {
            worker.emplace([snapshot = state] {
                std::unique_lock<std::mutex> lock(snapshot->mutex);
                snapshot->timed_out = !snapshot->changed.wait_for(lock, std::chrono::seconds(2),
                    [snapshot] { return snapshot->stopped; });
            });
        }
        if (throw_after == stage) throw std::runtime_error("injected init failure");
        return fail_at != stage;
    }
    void run_event_loop() override {
        entered_loop = true;
        if (throw_in_loop) throw std::runtime_error("injected loop failure");
    }
    void shutdown_step(TuiShutdownStep step) override {
        shutdowns.push_back(step);
        if (throw_on_shutdown == step) throw std::runtime_error("injected shutdown failure");
        if (step == TuiShutdownStep::AbortAndWake) stop_worker();
        if (step == TuiShutdownStep::AgentWorker && worker) worker->join();
    }
    void shutdown_error(TuiShutdownStep step) noexcept override { errors.push_back(step); }
private:
    void stop_worker() {
        { std::lock_guard<std::mutex> lock(state->mutex); state->stopped = true; }
        state->changed.notify_all();
    }
};
}
// 中文顺序契约：关停逐项与设计表一致，重复调用不重复释放/打印。
TEST(TuiShutdownSequence, PreservesEveryShutdownStepAndIsIdempotent) {
    const std::vector<TuiShutdownStep> expected{
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
    };
    LifecycleProbe probe;
    acecode::tui::TuiShutdownSequence shutdown;
    shutdown.run(probe);
    shutdown.run(probe);
    EXPECT_EQ(probe.shutdowns, expected);
}

// 中文异常契约：每个初始化阶段注入失败，共用真实编排路径；若 worker 已启动则先唤醒再 join。
TEST(TuiShutdownSequence, EveryInitializationStageUnwindsThroughSameSequence) {
    for (auto stage : acecode::tui::TuiInitSequence::order()) {
        SCOPED_TRACE(static_cast<int>(stage));
        LifecycleProbe probe;
        probe.throw_after = stage;
        acecode::tui::TuiShutdownSequence shutdown;
        EXPECT_THROW(acecode::tui::run_tui_application(probe, shutdown), std::runtime_error);
        EXPECT_FALSE(probe.entered_loop);
        EXPECT_EQ(probe.initialized.back(), stage);
        const auto& order = acecode::tui::TuiShutdownSequence::order();
        EXPECT_EQ(probe.shutdowns, (std::vector<TuiShutdownStep>(order.begin(), order.end())));
        if (probe.worker) EXPECT_FALSE(probe.worker->joinable());
        EXPECT_FALSE(probe.state->timed_out);
        EXPECT_TRUE(probe.errors.empty());
    }
}

// 中文回归说明：显式启动失败、循环异常、关停单步异常均不跳过后续资源释放。
TEST(TuiShutdownSequence, EarlyReturnAndLoopFailureStillReleaseOwners) {
    LifecycleProbe early;
    early.fail_at = TuiInitStage::Environment;
    acecode::tui::TuiShutdownSequence early_shutdown;
    EXPECT_EQ(acecode::tui::run_tui_application(early, early_shutdown), 1);
    EXPECT_FALSE(early.entered_loop);

    LifecycleProbe loop;
    loop.throw_in_loop = true;
    loop.throw_on_shutdown = TuiShutdownStep::Notifications;
    acecode::tui::TuiShutdownSequence loop_shutdown;
    EXPECT_THROW(acecode::tui::run_tui_application(loop, loop_shutdown), std::runtime_error);
    EXPECT_TRUE(loop.entered_loop);
    EXPECT_EQ(loop.shutdowns.back(), TuiShutdownStep::AbandonedWork);
    EXPECT_EQ(loop.errors, (std::vector<TuiShutdownStep>{TuiShutdownStep::Notifications}));
    ASSERT_TRUE(loop.worker);
    EXPECT_FALSE(loop.worker->joinable());
    EXPECT_FALSE(loop.state->timed_out);
}
