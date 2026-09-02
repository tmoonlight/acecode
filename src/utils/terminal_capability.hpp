#pragma once

// 终端能力探测 — 给 add-legacy-terminal-fallback 用。
//
// 启动时一次性读环境变量 + Windows 版本号,判断当前终端是否属于以下三类:
//   - Cmder/ConEmu(`ConEmuPID` 环境变量存在)
//   - Windows Terminal(`WT_SESSION` 存在)
//   - 老 Windows console(build < 17763,即 Win10 1809 之前)
//   - 传统 Windows Console Host(`conhost.exe` / ConsoleWindowClass)
//
// 这三个 bool 是独立信号,后续 src/tui/render_mode 的 decide_render_mode()
// 把它们组合到 ScreenRenderMode 决策。
//
// 为了单测,detect_terminal_capabilities_with() 接受可注入的 env / version
// 查询函数;detect_terminal_capabilities() 是用真实 getenv +
// RtlGetVersion + Win32 console window / VT probes 包好的产物函数。

#include <functional>
#include <optional>
#include <string>

namespace acecode {

struct TerminalCapabilities {
    bool is_conemu          = false;
    bool is_windows_terminal = false;
    bool is_legacy_conhost  = false;
    bool is_classic_conhost = false;
    // 给一次性提示用的来源标签,组合优先级:
    //   ConEmu 命中 → "Cmder/ConEmu"
    //   classic conhost → "Windows Console Host"
    //   仅 legacy conhost → "legacy Windows console"
    //   其它 → ""
    std::string source_label;
};

// 真实探测:读 getenv("ConEmuPID") / getenv("WT_SESSION") + RtlGetVersion +
// Win32 console host 信号。
TerminalCapabilities detect_terminal_capabilities();

// 测试专用:env_lookup 返回 std::optional<std::string>(nullopt = 未设置);
// version_lookup 返回 std::optional<unsigned> 表示 Windows build 号
// (nullopt = 不在 Windows / 探测失败 / 不应判定为 legacy)。
// classic_conhost_lookup 返回当前进程是否看起来跑在传统 Windows Console
// Host 下;如果为空,按 false 处理。
//
// 这是纯函数,不读任何全局状态,所有数据来自两个回调。
TerminalCapabilities detect_terminal_capabilities_with(
    const std::function<std::optional<std::string>(const char* name)>& env_lookup,
    const std::function<std::optional<unsigned>()>& version_lookup,
    const std::function<bool()>& classic_conhost_lookup = {});

inline bool should_use_conhost_compat_layout(const TerminalCapabilities& caps) {
    return !caps.is_windows_terminal &&
           (caps.is_classic_conhost || caps.is_legacy_conhost);
}

// 同步刷新(DEC mode 2026, CSI ?2026h / ?2026l)支持判定 —— 环境变量启发式。
//
// 决策表(黑名单优先):
//   - ConEmu/Cmder(ConEmuPID)                     → false
//   - legacy / classic conhost                    → false
//   - TERM 以 "tmux" / "screen" 开头(复用器)      → false
//   - WT_SESSION / KITTY_WINDOW_ID 存在            → true
//   - TERM_PROGRAM ∈ {iTerm.app, WezTerm, ghostty, vscode,
//                     WarpTerminal, contour, mintty} → true
//     (Apple_Terminal / macOS Terminal.app 不在此列:旧版本不支持 DEC 2026,
//      保守关闭;需用时用 tui.sync_output_mode="always" 强制开启)
//   - TERM == xterm-kitty 或以 foot/ghostty 开头    → true
//   - 其它(未知终端,如 Alacritty / 裸 xterm-256color) → false
//
// 未知默认关闭是刻意保守:不支持 2026 的终端(如老 conhost)如果不识别该
// 序列,行为不可控;支持方通过 tui.sync_output_mode="always" 可强制开启。
// 与 detect_terminal_capabilities_with 一样,env_lookup 可注入以便单测。
bool detect_synchronized_output_support_with(
    const TerminalCapabilities& caps,
    const std::function<std::optional<std::string>(const char* name)>& env_lookup);

// 真实探测:读当前进程环境变量。
bool detect_synchronized_output_support();

// OSC 8 超链接支持判定 —— 环境变量启发式,决策表与同步刷新一致
// (blacklist > whitelist > unknown-off),吸收自 markdown 渲染器里
// 从未被调用的 terminal_supports_hyperlinks() 死代码:
//   - ConEmu/Cmder(ConEmuPID)                       → false
//   - legacy / classic conhost                      → false
//   - TERM 以 "tmux" / "screen" 开头(复用器)        → false
//   - WT_SESSION / KITTY_WINDOW_ID 存在             → true
//   - TERM_PROGRAM ∈ {iTerm.app, WezTerm, ghostty, vscode,
//                     WarpTerminal, contour, mintty} → true
//     (Apple_Terminal / macOS Terminal.app 无 OSC 8,不进白名单)
//   - TERM == xterm-kitty 或以 foot/ghostty 开头     → true
//   - 其它(未知终端,如裸 xterm-256color)            → false
//
// 未知默认关闭。注意:死代码曾用 TERM 含 "xterm" 子串放行,这里收紧为
// 只认 xterm-kitty——裸 xterm-256color 被大量不支持的终端伪装,保守关。
// 即使误判发射 OSC 8 序列也无害(终端忽略),优雅降级为纯文本下划线。
bool detect_osc8_support_with(
    const TerminalCapabilities& caps,
    const std::function<std::optional<std::string>(const char* name)>& env_lookup);

// 真实探测:读当前进程环境变量。
bool detect_osc8_support();

// 悬停移动上报(DEC mode 1003, any-event)安全判定 —— 门控
// ftxui::App::EnableMouseHoverMotion()。决策表与 OSC 8 一致,但语义独立:
//   ?1003 在老式/经典 Windows conhost 上会触发重绘抖动(idle-mouse-redraw
//   补丁当初特意降为 ?1002 button-event 的动机),因此 conhost 家族强制关;
//   未知终端默认关闭。支持 OSC 8 的现代终端(iTerm2 / kitty / WezTerm /
//   ghostty / VS Code / Windows Terminal / Warp / contour / mintty)
//   对 any-event 上报同样支持,进白名单。
bool detect_hover_motion_support_with(
    const TerminalCapabilities& caps,
    const std::function<std::optional<std::string>(const char* name)>& env_lookup);

// 真实探测:读当前进程环境变量。
bool detect_hover_motion_support();

} // namespace acecode
