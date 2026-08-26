#include "terminal_capability.hpp"

#include "logger.hpp"

#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace acecode {

namespace {

// Win10 1809 = build 17763. 这是 conhost 真正修复 VT cursor-up 序列、
// 引入 disable_newline_auto_return 等 ENABLE_VIRTUAL_TERMINAL 行为的版本。
// build < 17763 视为 legacy。
constexpr unsigned kWin10_1809_Build = 17763;

#ifdef _WIN32
bool wide_equals_ignore_case(const std::wstring& lhs, const wchar_t* rhs) {
    const int result = CompareStringOrdinal(
        lhs.c_str(),
        static_cast<int>(lhs.size()),
        rhs,
        -1,
        TRUE);
    return result == CSTR_EQUAL;
}

// 用 RtlGetVersion 拿到真实 Windows build 号,绕开 GetVersionExW 的兼容性
// shim(那个会一直返回 6.2 即使在 Win11 上)。失败 → nullopt + LOG_WARN。
std::optional<unsigned> probe_windows_build() {
    HMODULE h = GetModuleHandleW(L"ntdll.dll");
    if (!h) {
        LOG_WARN("[terminal_capability] GetModuleHandleW(ntdll.dll) returned NULL");
        return std::nullopt;
    }
    using RtlGetVersionPtr = LONG (WINAPI*)(PRTL_OSVERSIONINFOW);
    auto fn = reinterpret_cast<RtlGetVersionPtr>(
        reinterpret_cast<void*>(GetProcAddress(h, "RtlGetVersion")));
    if (!fn) {
        LOG_WARN("[terminal_capability] GetProcAddress(RtlGetVersion) returned NULL");
        return std::nullopt;
    }
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (fn(&info) != 0) {
        LOG_WARN("[terminal_capability] RtlGetVersion call failed");
        return std::nullopt;
    }
    return static_cast<unsigned>(info.dwBuildNumber);
}

bool is_console_window_class(HWND hwnd) {
    wchar_t class_name[256] = {};
    constexpr int class_name_count = static_cast<int>(sizeof(class_name) / sizeof(class_name[0]));
    if (GetClassNameW(hwnd, class_name, class_name_count) <= 0) {
        return false;
    }
    return wide_equals_ignore_case(class_name, L"ConsoleWindowClass");
}

bool console_handle_supports_vt(DWORD std_handle) {
    HANDLE handle = GetStdHandle(std_handle);
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;

    DWORD mode = 0;
    if (!GetConsoleMode(handle, &mode)) return false;

    const DWORD vt_mode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(handle, vt_mode)) return false;

    SetConsoleMode(handle, mode);
    return true;
}

bool probe_classic_conhost() {
    HWND hwnd = GetConsoleWindow();
    if (!hwnd) return false;

    if (is_console_window_class(hwnd) && IsWindowVisible(hwnd)) {
        return true;
    }

    if (console_handle_supports_vt(STD_OUTPUT_HANDLE) ||
        console_handle_supports_vt(STD_ERROR_HANDLE)) {
        return false;
    }

    return true;
}
#else
std::optional<unsigned> probe_windows_build() {
    return std::nullopt; // 非 Windows 平台没有 build 号
}

bool probe_classic_conhost() {
    return false;
}
#endif

std::optional<std::string> default_env_lookup(const char* name) {
    const char* v = std::getenv(name);
    if (!v) return std::nullopt;
    return std::string(v);
}

// TERM_PROGRAM 白名单:已确认实现 DEC mode 2026 的终端。
// (Windows Terminal 走 WT_SESSION,kitty 走 KITTY_WINDOW_ID/TERM,不在此表。)
bool term_program_whitelisted(const std::string& value) {
    static const char* kKnown[] = {
        "iTerm.app",       // iTerm2
        "WezTerm",
        "ghostty",
        "vscode",          // VS Code 内嵌终端(xterm.js)
        "Apple_Terminal",  // macOS Terminal.app
        "WarpTerminal",
        "contour",
        "mintty",
    };
    for (const char* name : kKnown) {
        if (value == name) return true;
    }
    return false;
}

// TERM 白名单:kitty / foot / ghostty 会用 TERM 标记自己。
// ghostty 的实际默认值是 "xterm-ghostty",所以按任意位置匹配;foot 用前缀
// (foot / foot-256color)。
bool term_whitelisted(const std::string& term) {
    return term == "xterm-kitty" || term.find("ghostty") != std::string::npos ||
           term.rfind("foot", 0) == 0;
}

// TERM 黑名单:复用器版本无法从环境判定,保守关闭(可配置强制开)。
bool term_blacklisted(const std::string& term) {
    return term.rfind("tmux", 0) == 0 || term.rfind("screen", 0) == 0;
}

} // namespace

TerminalCapabilities detect_terminal_capabilities_with(
    const std::function<std::optional<std::string>(const char* name)>& env_lookup,
    const std::function<std::optional<unsigned>()>& version_lookup,
    const std::function<bool()>& classic_conhost_lookup) {
    TerminalCapabilities caps;

    // ConEmu/Cmder: 任何非空值都视为命中(ConEmu 设置的是 PID 字符串)。
    auto conemu = env_lookup("ConEmuPID");
    if (conemu.has_value() && !conemu->empty()) {
        caps.is_conemu = true;
    }

    // Windows Terminal: WT_SESSION 是 GUID 字符串。同样任何非空都视为命中。
    auto wt = env_lookup("WT_SESSION");
    if (wt.has_value() && !wt->empty()) {
        caps.is_windows_terminal = true;
    }

    // 老 conhost: 只有拿到 build 号且小于 17763 才标记为 legacy。
    auto build = version_lookup();
    if (build.has_value() && *build < kWin10_1809_Build) {
        caps.is_legacy_conhost = true;
    }

    if (!caps.is_windows_terminal && classic_conhost_lookup &&
        classic_conhost_lookup()) {
        caps.is_classic_conhost = true;
    }

    // 来源标签优先级:ConEmu > classic conhost > legacy conhost > 空。
    if (caps.is_conemu) {
        caps.source_label = "Cmder/ConEmu";
    } else if (caps.is_classic_conhost) {
        caps.source_label = "Windows Console Host";
    } else if (caps.is_legacy_conhost) {
        caps.source_label = "legacy Windows console";
    }

    return caps;
}

TerminalCapabilities detect_terminal_capabilities() {
    return detect_terminal_capabilities_with(
        default_env_lookup, probe_windows_build, probe_classic_conhost);
}

bool detect_synchronized_output_support_with(
    const TerminalCapabilities& caps,
    const std::function<std::optional<std::string>(const char* name)>& env_lookup) {
    // 黑名单优先:任何一条命中都关闭。
    auto conemu = env_lookup("ConEmuPID");
    if (conemu.has_value() && !conemu->empty()) {
        return false;
    }
    if (caps.is_legacy_conhost || caps.is_classic_conhost) {
        return false;
    }
    auto term = env_lookup("TERM");
    if (term.has_value() && term_blacklisted(*term)) {
        return false;
    }

    // 白名单:命中任意一条即开启。
    auto wt_session = env_lookup("WT_SESSION");
    if (wt_session.has_value() && !wt_session->empty()) {
        return true;  // Windows Terminal
    }
    auto kitty_window_id = env_lookup("KITTY_WINDOW_ID");
    if (kitty_window_id.has_value() && !kitty_window_id->empty()) {
        return true;  // kitty
    }
    auto term_program = env_lookup("TERM_PROGRAM");
    if (term_program.has_value() && term_program_whitelisted(*term_program)) {
        return true;
    }
    if (term.has_value() && term_whitelisted(*term)) {
        return true;
    }

    // 未知终端:默认关闭(保守)。
    return false;
}

bool detect_synchronized_output_support() {
    // 先跑一次完整能力探测,确保 legacy/classic conhost 黑名单在真实环境下生效
    // (不能传空 TerminalCapabilities,否则 Windows 上的黑名单分支永远不会命中)。
    return detect_synchronized_output_support_with(detect_terminal_capabilities(),
                                                   default_env_lookup);
}

} // namespace acecode
