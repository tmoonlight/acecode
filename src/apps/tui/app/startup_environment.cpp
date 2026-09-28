#include "startup_environment.hpp"
#include "startup_worktree.hpp"
#include "tui/term/terminal_control.hpp"
#include "cli/interactive_options.hpp"
#include "utils/logger.hpp"
#include "utils/paths.hpp"
#include "workspace/workspace_registry.hpp"
#include "version.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <direct.h>
#else
#include <termios.h>
#include <unistd.h>
#endif
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace acecode::tui {

static std::string get_cwd() {
#ifdef _WIN32
    char buf[MAX_PATH];
    if (_getcwd(buf, sizeof(buf))) return std::string(buf);
#else
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) return std::string(buf);
#endif
    return ".";
}

static bool ensure_interactive_terminal() {
    std::atexit(term::reset_cursor);

#ifdef _WIN32
    bool stdin_is_tty = _isatty(_fileno(stdin));
    bool stdout_is_tty = _isatty(_fileno(stdout));
#else
    bool stdin_is_tty = isatty(fileno(stdin));
    bool stdout_is_tty = isatty(fileno(stdout));
#endif
    if (!stdin_is_tty || !stdout_is_tty) {
        std::cerr << "Error: acecode requires an interactive terminal (stdin and stdout must be a TTY).\n"
                  << "If piping input/output, please run acecode directly in a terminal instead.\n";
        return false;
    }
    return true;
}

static void set_startup_terminal_title() {
#ifdef _WIN32
    SetConsoleTitleA("acecode v" ACECODE_VERSION);
#else
    // xterm-compatible title escape sequence
    std::cout << "\033]0;acecode v" ACECODE_VERSION "\007" << std::flush;
#endif
}

static void initialize_logger_for_working_dir(const std::string& working_dir) {
    const std::string logs_dir = get_logs_dir();
    Logger::instance().init_with_rotation(logs_dir, "tui", /*mirror_stderr=*/false);
    // 数据目录重定向的解析告警发生在日志初始化之前(被 Logger 丢掉),这里补记。
    acecode::log_deferred_data_dir_resolution_warning();
#ifdef _WIN32
    _putenv_s("ACECODE_FTXUI_INPUT_TRACE_DIR", logs_dir.c_str());
#else
    setenv("ACECODE_FTXUI_INPUT_TRACE_DIR", logs_dir.c_str(), 1);
#endif
    Logger::instance().set_level(LogLevel::Dbg);
    LOG_INFO("=== acecode started, cwd=" + working_dir + " ===");
}

bool initialize_tui_startup_environment(std::string& working_dir,
                                               const InteractiveCliOptions& cli,
                                               WorktreeSessionInfo& startup_worktree,
                                               std::string& startup_worktree_banner) {
    if (!ensure_interactive_terminal()) {
        return false;
    }
    set_startup_terminal_title();
    working_dir = get_cwd();
    if (cli.worktree_enabled &&
        !bootstrap_startup_worktree(cli, working_dir, startup_worktree,
                                    startup_worktree_banner)) {
        return false;
    }
    initialize_logger_for_working_dir(working_dir);
    acecode::desktop::ensure_workspace_metadata(
        (std::filesystem::path(get_acecode_dir()) / "projects").string(),
        working_dir);
    return true;
}


} // namespace acecode::tui
