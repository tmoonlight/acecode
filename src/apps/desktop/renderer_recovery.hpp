#pragma once

#include <cstdint>
#include <deque>

namespace acecode::desktop {

// 主窗口 WebView 渲染进程故障后的自动恢复判定(纯逻辑,无平台依赖)。
//
// 背景:Desktop 挂机一两天后渲染进程因内存耗尽退出,WebView2 只显示自带的
// 「此页面出现问题 / 内存不足」错误页,用户必须手动点刷新(Chrome / Edge
// 标签页同款行为)。宿主拿得到 ProcessFailed 事件,渲染进程退出时可以直接
// Reload() 把页面带回来;但页面若一加载就崩(例如某个会话一打开就爆内存),
// 无条件重载会变成死循环,所以按时间窗限次:窗口内超过上限就放弃,保留错误页
// 让用户自己处理。
enum class RendererFailure {
    // 主页面的渲染进程退出(含 OOM 被杀、崩溃)。错误页已显示,Reload 能恢复。
    RenderProcessExited,
    // 渲染进程无响应。可能只是一次长任务或解锁后的 GC 风暴,自己会缓过来;
    // 若真的撑爆会再以 RenderProcessExited 上报,届时再重载。
    RenderProcessUnresponsive,
    // 浏览器主进程退出:整个 WebView 已失效,Reload 无效,需要重建 WebView。
    BrowserProcessExited,
    // GPU / 工具进程 / iframe 渲染进程等,WebView2 会自行重启或不影响主页面。
    Other,
};

enum class RendererRecoveryAction {
    Reload,  // 重新加载当前页面
    Ignore,  // 不处理(不影响主页面,或交给 WebView2 自行恢复)
    GiveUp,  // 时间窗内重载过多,保留错误页
};

struct RendererRecoveryLimits {
    int max_reloads = 3;
    std::int64_t window_ms = 10 * 60 * 1000;
};

class RendererRecoveryTracker {
public:
    explicit RendererRecoveryTracker(RendererRecoveryLimits limits = {});

    // 记录一次故障并给出处置。只有返回 Reload 时才计入时间窗。
    RendererRecoveryAction on_failure(RendererFailure failure, std::int64_t now_ms);

private:
    RendererRecoveryLimits limits_;
    std::deque<std::int64_t> reloads_ms_;
};

} // namespace acecode::desktop
