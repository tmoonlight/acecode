// renderer_recovery 纯逻辑单测。
//
// 背景:用户反馈 Desktop 挂机一两天后解锁屏幕,窗口停在 WebView2 自带的
// 「内存不足」错误页,必须手动点刷新。宿主现在监听 ProcessFailed,主页面渲染
// 进程退出时自动 Reload;这里守住「什么故障才重载」与「防崩溃循环的限次」。

#include "desktop/renderer_recovery.hpp"

#include <gtest/gtest.h>

namespace {

using acecode::desktop::RendererFailure;
using acecode::desktop::RendererRecoveryAction;
using acecode::desktop::RendererRecoveryLimits;
using acecode::desktop::RendererRecoveryTracker;

constexpr std::int64_t kMinute = 60 * 1000;

// 场景:挂机后渲染进程因内存耗尽退出(WebView2 报 RENDER_PROCESS_EXITED)。
// 期望:第一次故障直接重载,用户解锁后看到的是正常页面而不是错误页。
TEST(RendererRecovery, RenderProcessExitReloads) {
    RendererRecoveryTracker tracker;
    EXPECT_EQ(tracker.on_failure(RendererFailure::RenderProcessExited, 0),
              RendererRecoveryAction::Reload);
}

// 场景:渲染进程短暂无响应(长任务或解锁后的 GC 风暴)、浏览器主进程退出、
// GPU / 工具进程等非主页面故障。
// 期望:都不重载 —— 无响应可能自己缓过来,真撑爆会再报 RenderProcessExited;
// 浏览器主进程退出后 Reload 无效;其它进程由 WebView2 自行恢复。
TEST(RendererRecovery, NonRenderExitFailuresAreIgnored) {
    RendererRecoveryTracker tracker;
    EXPECT_EQ(tracker.on_failure(RendererFailure::RenderProcessUnresponsive, 0),
              RendererRecoveryAction::Ignore);
    EXPECT_EQ(tracker.on_failure(RendererFailure::BrowserProcessExited, 0),
              RendererRecoveryAction::Ignore);
    EXPECT_EQ(tracker.on_failure(RendererFailure::Other, 0),
              RendererRecoveryAction::Ignore);
}

// 场景:页面一加载就崩(例如某个会话一打开就爆内存),渲染进程在 10 分钟内
// 连续退出。
// 期望:前 3 次重载,第 4 次放弃并保留错误页,不陷入无限重载循环。
TEST(RendererRecovery, CrashLoopGivesUpAfterLimit) {
    RendererRecoveryTracker tracker;
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(tracker.on_failure(RendererFailure::RenderProcessExited, i * kMinute),
                  RendererRecoveryAction::Reload);
    }
    EXPECT_EQ(tracker.on_failure(RendererFailure::RenderProcessExited, 3 * kMinute),
              RendererRecoveryAction::GiveUp);
}

// 场景:几天里偶尔各崩一次(每次间隔超过时间窗)。
// 期望:旧的重载记录滑出时间窗,之后的故障仍然自动重载。
TEST(RendererRecovery, OldReloadsLeaveTheWindow) {
    RendererRecoveryTracker tracker(RendererRecoveryLimits{2, 10 * kMinute});
    EXPECT_EQ(tracker.on_failure(RendererFailure::RenderProcessExited, 0),
              RendererRecoveryAction::Reload);
    EXPECT_EQ(tracker.on_failure(RendererFailure::RenderProcessExited, 1 * kMinute),
              RendererRecoveryAction::Reload);
    EXPECT_EQ(tracker.on_failure(RendererFailure::RenderProcessExited, 2 * kMinute),
              RendererRecoveryAction::GiveUp);
    // 第一次重载(t=0)在 t=10min 恰好滑出,窗口里只剩 1 次。
    EXPECT_EQ(tracker.on_failure(RendererFailure::RenderProcessExited, 10 * kMinute),
              RendererRecoveryAction::Reload);
}

// 场景:放弃之后的故障。
// 期望:GiveUp 不占用重载名额,时间窗过去后能重新恢复自动重载。
TEST(RendererRecovery, GiveUpDoesNotConsumeQuota) {
    RendererRecoveryTracker tracker(RendererRecoveryLimits{1, 10 * kMinute});
    EXPECT_EQ(tracker.on_failure(RendererFailure::RenderProcessExited, 0),
              RendererRecoveryAction::Reload);
    EXPECT_EQ(tracker.on_failure(RendererFailure::RenderProcessExited, 1 * kMinute),
              RendererRecoveryAction::GiveUp);
    EXPECT_EQ(tracker.on_failure(RendererFailure::RenderProcessExited, 5 * kMinute),
              RendererRecoveryAction::GiveUp);
    EXPECT_EQ(tracker.on_failure(RendererFailure::RenderProcessExited, 11 * kMinute),
              RendererRecoveryAction::Reload);
}

} // namespace
