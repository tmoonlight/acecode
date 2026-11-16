#pragma once

#include <functional>

namespace acecode::desktop {

// 显示主窗口并把它放到最前。用于「不是用户亲手启动」的那次启动:升级后旧进程
// 拉起的新进程、新版本装好后的第一次启动。
//
// 背景(Windows 前台锁):进程只有在自己是前台进程、或由前台进程启动时,
// SetForegroundWindow 才会被系统接受。升级重启时,旧进程要先销毁窗口、停完
// daemon 才拉起新进程 —— 那一刻它早已不是前台进程,新窗口于是出现在别的窗口
// 后面,只在任务栏闪。这里先临时并入当前前台线程的输入队列再激活;仍被拒绝
// (前台程序无响应或权限更高)就用一次置顶脉冲把窗口抬到最上层后立刻撤销。
//
// show 负责让窗口可见(WebHost::set_visible(true)),在本次调用内同步执行一次。
// 必须在窗口所属的 UI 线程调用。返回窗口最终是否在最前;非 Windows 平台只执行
// show(macOS 的激活由 LaunchServices 负责,见 desktop_restart)。
bool show_window_in_foreground(void* native_window, const std::function<void()>& show);

} // namespace acecode::desktop
