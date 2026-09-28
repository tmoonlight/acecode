#pragma once

#include "platform/process/unique_resources.hpp"

// Final Windows fallback for acecode-desktop startup. If embedded WebView2
// cannot be initialized, the desktop process can keep the supervised daemon
// alive and show the daemon web UI through Microsoft Edge app mode.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace acecode::desktop {

struct EdgeAppLaunchResult {
    bool ok = false;
    std::string error;
    unsigned long exit_code = 0;
};

// 非阻塞启动的结果:进程句柄交给调用方,由调用方决定何时 wait / 关句柄。
// Windows 进程句柄由 move-only 结果持有,销毁时自动关闭。
struct EdgeAppLaunchHandle {
    bool ok = false;
    std::string error;
#ifdef _WIN32
    platform::UniqueHandle process;
#endif
    unsigned long pid = 0;
};

// Pure helper used by unit tests. Each root is expected to be a ProgramFiles-like
// directory; the function checks Microsoft/Edge/Application/msedge.exe.
std::optional<std::filesystem::path> find_msedge_executable_in(
    const std::vector<std::filesystem::path>& roots);

// Windows system lookup. POSIX builds return nullopt.
std::optional<std::filesystem::path> find_msedge_executable();

// Build ShellExecute/CreateProcess-style parameters for Edge app mode. Exposed
// for tests because quoting the user-data-dir path is easy to regress.
std::wstring build_edge_app_parameters_w(const std::wstring& url,
                                         const std::wstring& user_data_dir);

// 纯函数:每次启动用的 user-data-dir 子目录名(按进程 id 唯一化)。让本次启动
// 的 msedge 永远找不到可转交的旧实例,从而避免 Chromium single-instance 把
// URL 转交给残留进程后自身立刻退出(那会让调用方误判窗口已关、提前杀 daemon →
// 白屏)。Exposed for tests。
std::string edge_profile_subdir_name(unsigned long pid);

// 非阻塞启动 Edge app 模式:用一个干净的 per-launch user-data-dir,启动后立刻
// 返回进程句柄(不等待退出)。调用方负责 wait 与 daemon 清理,句柄自动释放。
// 这样 daemon 的生命周期可以由调用方用更可靠的方式(托盘 + 进程 watcher)托管,
// 而不是绑死在一个对 Chromium 不可靠的进程句柄等待上。POSIX 返回 error。
EdgeAppLaunchHandle launch_edge_app(const std::string& url);

// Launch Edge in app mode, wait for the app process to exit, and return its
// exit code. The caller remains responsible for daemon cleanup. 现在内部走
// launch_edge_app(同样用干净 profile),保留给可能的旧调用方。
EdgeAppLaunchResult launch_edge_app_and_wait(const std::string& url);

} // namespace acecode::desktop
