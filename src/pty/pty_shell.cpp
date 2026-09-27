// resolve_console_shell / pty_backend_kind_name / detect_console_shells:平台无关
// 纯逻辑,进 acecode_testable 供单测直接覆盖(spawn 实现在 pty_backend_{win,posix}.cpp)。
//
// detect_console_shells 只做**目录层面**的探测(文件在不在、环境变量、注册表),
// 并把每个类型的 program / detected_path / configured_path / fallback_programs
// 交给 environment::resolve_terminal 做真正的启动探测与逐级回退。

#include "pty_backend.hpp"

#include "utils/encoding.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <pwd.h>
#  include <unistd.h>
#endif

namespace acecode {

#ifndef _WIN32
namespace {
// GUI 启动的 daemon(桌面壳 / launchd)环境里往往没有 $SHELL;
// 从 passwd 数据库拿用户真正的登录 shell(VS Code 同此回退)。
std::string posix_login_shell() {
    if (const struct passwd* pw = getpwuid(getuid()); pw && pw->pw_shell && pw->pw_shell[0]) {
        return pw->pw_shell;
    }
    return {};
}
}  // namespace
#endif

const char* pty_backend_kind_name(PtyBackendKind kind) {
    switch (kind) {
        case PtyBackendKind::ConPty:   return "conpty";
        case PtyBackendKind::Winpty:   return "winpty";
        case PtyBackendKind::Pipe:     return "pipe";
        case PtyBackendKind::PosixPty: return "posix";
    }
    return "unknown";
}

std::string resolve_console_shell(const std::string& configured) {
    if (!configured.empty()) return configured;
#ifdef _WIN32
    std::string comspec = getenv_utf8("COMSPEC");
    return comspec.empty() ? "cmd.exe" : comspec;
#else
    std::string shell = getenv_utf8("SHELL");
    if (!shell.empty()) return shell;
    shell = posix_login_shell();
    return shell.empty() ? "/bin/sh" : shell;
#endif
}

// ── 控制台 shell 目录探测 ───────────────────────────────────────────────

bool is_wsl_system32_bash(const std::string& path) {
    std::string lower;
    lower.reserve(path.size());
    for (char ch : path) {
        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        lower.push_back(c == '/' ? '\\' : c);
    }
    return lower.find("\\system32\\bash.exe") != std::string::npos;
}

std::string quote_shell_path_if_needed(const std::string& path) {
    if (path.find(' ') == std::string::npos) return path;
    return "\"" + path + "\"";
}

namespace {

#ifdef _WIN32
std::string win_git_install_path_from_registry() {
    const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
    for (HKEY root : roots) {
        HKEY key = nullptr;
        if (::RegOpenKeyExW(root, L"SOFTWARE\\GitForWindows", 0, KEY_READ, &key) !=
                ERROR_SUCCESS || !key) {
            continue;
        }
        wchar_t buf[1024];
        DWORD bytes = sizeof(buf);
        DWORD type = 0;
        LONG rc = ::RegQueryValueExW(key, L"InstallPath", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(buf), &bytes);
        ::RegCloseKey(key);
        if (rc == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) &&
                bytes >= sizeof(wchar_t)) {
            std::wstring w(buf, bytes / sizeof(wchar_t));
            while (!w.empty() && w.back() == L'\0') w.pop_back();
            if (!w.empty()) return wide_to_utf8(w);
        }
    }
    return {};
}
#endif

std::string configured_path_for(const ShellPaths& paths, const std::string& id) {
    auto it = paths.find(id);
    return it == paths.end() ? std::string{} : it->second;
}

// 显式路径存在时优先当 program;返回是否采用了显式路径。显式路径不存在时只记在
// configured_path 上(resolve_terminal 会把"配置路径不存在"报成回退原因)。
bool apply_configured_program(ConsoleShellOption& opt, const ShellPaths& paths,
                              const ShellProbe& probe) {
    opt.configured_path = configured_path_for(paths, opt.id);
    if (opt.configured_path.empty()) return false;
    if (!probe.exists(opt.configured_path)) return false;
    opt.program = opt.configured_path;
    return true;
}

}  // namespace

ShellProbe default_shell_probe() {
    ShellProbe p;
    p.exists = [](const std::string& path) {
        if (path.empty()) return false;
        std::error_code ec;
        return std::filesystem::exists(std::filesystem::u8path(path), ec);
    };
    p.getenv = [](const std::string& name) { return getenv_utf8(name.c_str()); };
#ifdef _WIN32
    p.git_install_path = []() { return win_git_install_path_from_registry(); };
    p.login_shell = []() { return std::string{}; };
#else
    p.git_install_path = []() { return std::string{}; };
    p.login_shell = []() { return posix_login_shell(); };
#endif
    return p;
}

std::vector<ConsoleShellOption> detect_console_shells(
    const ShellPaths& shell_paths, const ShellProbe& probe) {
    std::vector<ConsoleShellOption> out;
#ifdef _WIN32
    // PowerShell:显式路径 > pwsh(7)> powershell.exe(System32 必有)。
    // 同类备选按 pwsh → powershell.exe 排,启动探测失败时逐个退。
    {
        ConsoleShellOption ps;
        ps.id = "powershell";
        ps.label = "PowerShell";
        std::vector<std::string> pwsh_candidates;
        const auto search_path = probe.getenv("PATH");
        for (std::size_t pos = 0; pos < search_path.size();) {
            const auto end = search_path.find(';', pos);
            const auto directory = search_path.substr(pos, end == std::string::npos ? end : end - pos);
            if (!directory.empty()) pwsh_candidates.push_back(directory + "\\pwsh.exe");
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        const std::string program_files = probe.getenv("ProgramFiles");
        const std::string local_appdata = probe.getenv("LocalAppData");
        if (!program_files.empty())
            pwsh_candidates.push_back(program_files + "\\PowerShell\\7\\pwsh.exe");
        if (!local_appdata.empty())
            pwsh_candidates.push_back(local_appdata + "\\Microsoft\\WindowsApps\\pwsh.exe");
        std::string pwsh;
        for (const auto& c : pwsh_candidates) {
            if (probe.exists(c)) { pwsh = c; break; }
        }
        std::string windows_powershell = "powershell.exe";
        const auto system_root = probe.getenv("SystemRoot");
        const auto system_powershell = system_root + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
        if (!system_root.empty() && probe.exists(system_powershell)) windows_powershell = system_powershell;
        ps.detected_path = pwsh.empty() ? windows_powershell : pwsh;
        if (!apply_configured_program(ps, shell_paths, probe)) {
            ps.program = ps.detected_path;
            if (!pwsh.empty()) ps.label = "PowerShell 7";
        }
        if (!pwsh.empty() && ps.program != pwsh) ps.fallback_programs.push_back(pwsh);
        if (ps.program != windows_powershell) ps.fallback_programs.push_back(windows_powershell);
        ps.command = quote_shell_path_if_needed(ps.program);
        ps.available = true;
        out.push_back(std::move(ps));
    }
    // Git Bash:显式路径 → 常见安装位置 → 注册表 InstallPath。排除 WSL。
    {
        ConsoleShellOption gb;
        gb.id = "git-bash";
        gb.label = "Git Bash";
        gb.configured_path = configured_path_for(shell_paths, gb.id);
        std::vector<std::string> candidates;
        auto add_git_root = [&](const std::string& base) {
            if (!base.empty()) candidates.push_back(base + "\\Git\\bin\\bash.exe");
        };
        add_git_root(probe.getenv("ProgramFiles"));
        add_git_root(probe.getenv("ProgramW6432"));
        add_git_root(probe.getenv("ProgramFiles(x86)"));
        if (std::string la = probe.getenv("LocalAppData"); !la.empty())
            candidates.push_back(la + "\\Programs\\Git\\bin\\bash.exe");
        if (probe.git_install_path) {
            if (std::string gip = probe.git_install_path(); !gip.empty())
                candidates.push_back(gip + "\\bin\\bash.exe");
        }
        for (const auto& c : candidates) {
            if (is_wsl_system32_bash(c)) continue;  // WSL 的 bash.exe 不是 Git Bash
            if (probe.exists(c)) { gb.detected_path = c; break; }
        }
        if (!gb.configured_path.empty() && !is_wsl_system32_bash(gb.configured_path) &&
                probe.exists(gb.configured_path)) {
            gb.program = gb.configured_path;
        } else {
            gb.program = gb.detected_path;
        }
        if (!gb.program.empty()) {
            gb.command = quote_shell_path_if_needed(gb.program) + " --login -i";
            gb.available = true;
        } else {
            gb.available = false;
            gb.needs_path = true;
        }
        out.push_back(std::move(gb));
    }
    // cmd:显式路径 > %COMSPEC% > cmd.exe。
    {
        ConsoleShellOption c;
        c.id = "cmd";
        c.label = "Command Prompt";
        const std::string comspec = probe.getenv("COMSPEC");
        c.detected_path = comspec.empty() ? std::string("cmd.exe") : comspec;
        if (!apply_configured_program(c, shell_paths, probe)) c.program = c.detected_path;
        c.command = quote_shell_path_if_needed(c.program);
        c.available = true;
        out.push_back(std::move(c));
    }
#else
    // 默认 $SHELL → passwd 登录 shell → /bin/sh。
    {
        ConsoleShellOption def;
        def.id = "shell";
        def.label = "Default Shell";
        std::string sh = probe.getenv("SHELL");
        if (sh.empty() && probe.login_shell) sh = probe.login_shell();
        def.detected_path = sh.empty() ? std::string("/bin/sh") : sh;
        // 通用 /bin/sh 兜底不在这里加:见下方候选构建完成后的二次判定。
        if (!apply_configured_program(def, shell_paths, probe)) def.program = def.detected_path;
        def.command = def.program;
        def.available = true;
        out.push_back(std::move(def));
    }
    struct Cand { const char* id; const char* label; std::vector<std::string> paths; };
    const std::vector<Cand> cands = {
        {"bash", "Bash",
         {"/bin/bash", "/usr/bin/bash", "/usr/local/bin/bash", "/opt/homebrew/bin/bash"}},
        {"zsh", "Zsh",
         {"/bin/zsh", "/usr/bin/zsh", "/usr/local/bin/zsh", "/opt/homebrew/bin/zsh"}},
        {"fish", "Fish",
         {"/usr/bin/fish", "/usr/local/bin/fish", "/opt/homebrew/bin/fish"}},
    };
    for (const auto& cand : cands) {
        ConsoleShellOption o;
        o.id = cand.id;
        o.label = cand.label;
        for (const auto& p : cand.paths) {
            if (probe.exists(p)) { o.detected_path = p; break; }
        }
        if (!apply_configured_program(o, shell_paths, probe)) o.program = o.detected_path;
        o.command = o.program;
        o.available = !o.program.empty();
        o.needs_path = !o.available;
        out.push_back(std::move(o));
    }
    // 通用 /bin/sh 兜底只在"没有任何命名 shell(bash/zsh/fish)可用"时才挂到
    // 默认 shell 候选上。否则登录 shell 损坏时应回退到真实的 bash 等,
    // 而不是名不副实的 /bin/sh(见 BrokenLoginShellFallsBackToBash 用例)。
    if (!std::any_of(out.begin(), out.end(), [](const ConsoleShellOption& o) {
            return (o.id == "bash" || o.id == "zsh" || o.id == "fish") && o.available;
        })) {
        for (auto& o : out) {
            if (o.id == "shell" && o.detected_path != "/bin/sh")
                o.fallback_programs.push_back("/bin/sh");
        }
    }
#endif
    return out;
}

std::vector<ConsoleShellOption> detect_console_shells(const ShellPaths& shell_paths) {
    return detect_console_shells(shell_paths, default_shell_probe());
}

namespace {
ShellPaths legacy_shell_paths(const std::string& configured_git_bash_path) {
    ShellPaths paths;
    if (!configured_git_bash_path.empty()) paths["git-bash"] = configured_git_bash_path;
    return paths;
}
}  // namespace

std::vector<ConsoleShellOption> detect_console_shells(
    const std::string& configured_git_bash_path, const ShellProbe& probe) {
    return detect_console_shells(legacy_shell_paths(configured_git_bash_path), probe);
}

std::vector<ConsoleShellOption> detect_console_shells(
    const std::string& configured_git_bash_path) {
    return detect_console_shells(legacy_shell_paths(configured_git_bash_path),
                                 default_shell_probe());
}

std::optional<std::string> resolve_shell_command_by_id(
    const std::string& id, const ShellPaths& shell_paths, const ShellProbe& probe) {
    if (id.empty()) return std::nullopt;
    for (const auto& opt : detect_console_shells(shell_paths, probe)) {
        if (opt.id == id) {
            if (opt.available && !opt.command.empty()) return opt.command;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::string> resolve_shell_command_by_id(
    const std::string& id, const ShellPaths& shell_paths) {
    return resolve_shell_command_by_id(id, shell_paths, default_shell_probe());
}

std::optional<std::string> resolve_shell_command_by_id(
    const std::string& id, const std::string& configured_git_bash_path,
    const ShellProbe& probe) {
    return resolve_shell_command_by_id(id, legacy_shell_paths(configured_git_bash_path), probe);
}

std::optional<std::string> resolve_shell_command_by_id(
    const std::string& id, const std::string& configured_git_bash_path) {
    return resolve_shell_command_by_id(id, legacy_shell_paths(configured_git_bash_path),
                                       default_shell_probe());
}

std::string default_console_shell_id(
    const std::string& configured_default_shell,
    const ShellPaths& shell_paths, const ShellProbe& probe) {
    if (!configured_default_shell.empty()) {
        for (const auto& opt : detect_console_shells(shell_paths, probe)) {
            if (opt.id == configured_default_shell && opt.available) {
                return configured_default_shell;
            }
        }
    }
#ifdef _WIN32
    return "cmd";
#else
    return "shell";
#endif
}

std::string default_console_shell_id(
    const std::string& configured_default_shell, const ShellPaths& shell_paths) {
    return default_console_shell_id(configured_default_shell, shell_paths,
                                    default_shell_probe());
}

std::string default_console_shell_id(
    const std::string& configured_default_shell,
    const std::string& configured_git_bash_path, const ShellProbe& probe) {
    return default_console_shell_id(configured_default_shell,
                                    legacy_shell_paths(configured_git_bash_path), probe);
}

std::string default_console_shell_id(
    const std::string& configured_default_shell,
    const std::string& configured_git_bash_path) {
    return default_console_shell_id(configured_default_shell,
                                    legacy_shell_paths(configured_git_bash_path),
                                    default_shell_probe());
}

} // namespace acecode
