#include "command_dispatch.hpp"
#include "platform/utf8_command_line.hpp"
#include "upgrade/upgrade_cli_args.hpp"
#include "upgrade/upgrade.hpp"
#include "upgrade/apply.hpp"
#include "upgrade/manifest.hpp"
#include "headless/headless_options.hpp"
#include "headless/headless_runner.hpp"
#include "channels_cli.hpp"
#include "config/config.hpp"
#include "daemon/cli.hpp"
#include "web/remote_web_proxy.hpp"
#include "version.hpp"
#include <iostream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "daemon/service_win.hpp"
#endif

namespace acecode::cli {

static std::string executable_path_from_argv(int argc, char* argv[]) {
    return (argc > 0 && argv[0]) ? std::string(argv[0]) : std::string();
}

bool is_version_command_arg(const std::string& arg) {
    return arg == "version" || arg == "-version" || arg == "--version" ||
           arg == "/version";
}

bool is_help_command_arg(const std::string& arg) {
    return arg == "help" || arg == "-h" || arg == "--help" || arg == "/?";
}

// 顶层 `acecode --help`。各子命令的完整帮助由子命令自己出
// (`acecode -p --help` / `acecode daemon help`),这里只给导航级概览。
static void print_top_level_help() {
    std::cout <<
        "ACECode v" ACECODE_VERSION " - terminal coding agent\n"
        "\n"
        "Usage:\n"
        "  acecode [options]                  Start the interactive TUI\n"
        "  acecode -p [options] \"<prompt>\"    Headless print mode (acecode -p --help)\n"
        "  acecode configure                  Interactive provider/model setup\n"
        "  acecode daemon <subcommand>        Background daemon + Web UI (acecode daemon help)\n"
        "  acecode channels <command>         WhatsApp channel management (acecode channels help)\n"
#ifdef _WIN32
        "  acecode service <subcommand>       Windows service management (acecode service help)\n"
#endif
        "  acecode upgrade [--force]          Self-update to the latest release\n"
        "  acecode version                    Print version and exit\n"
        "\n"
        "Interactive (TUI) options:\n"
        "  --resume [id]          Resume a session (no id = most recent)\n"
        "  -r                     Open the resume picker on startup\n"
        "  -w, --worktree [name]  Work inside an isolated git worktree\n"
        "                         (name may also be a PR ref: #123 / GitHub PR URL)\n"
        "  --yolo, --dangerous    Skip all permission confirmations\n"
        "  --alt-screen           Force alternate-screen rendering (legacy terminals)\n"
        "\n"
        "Headless print mode (full reference: acecode -p --help):\n"
        "  acecode -p \"prompt\"                Run one turn, print the reply to stdout\n"
        "  echo \"...\" | acecode -p            Prompt from stdin (pipe-friendly)\n"
        "  acecode -p -c \"...\"                Continue this directory's latest session\n"
        "  acecode -p --resume <id> \"...\"     Continue a specific session\n"
        "  acecode -p --output-format json    Structured result with session_id\n";
}

std::optional<int> dispatch_non_tui_command(int argc, char* argv[]) {
    const std::string exe_path = executable_path_from_argv(argc, argv);

    if (argc >= 2 && std::string(argv[1]) == "--remote-web-proxy") {
        return acecode::web::run_remote_web_proxy_command(
            platform::argv_tail(argc, argv, 2), std::cout, std::cerr);
    }

    if (argc >= 2 && is_version_command_arg(argv[1] ? std::string(argv[1]) : std::string())) {
        std::cout << "acecode v" ACECODE_VERSION << "\n";
        return 0;
    }

    if (argc >= 2 && is_help_command_arg(argv[1] ? std::string(argv[1]) : std::string())) {
        print_top_level_help();
        return 0;
    }

    if (argc >= 2 && (std::string(argv[1]) == "upgrade" ||
                      std::string(argv[1]) == "update")) {
        bool force_update = false;
        std::optional<std::string> server_override;
        if (auto exit_code = upgrade::parse_upgrade_cli_args(
                argc, argv, force_update, server_override, std::cerr)) {
            return *exit_code;
        }
        AppConfig config = load_config();
        if (server_override.has_value()) {
            std::string server_error;
            if (!acecode::upgrade::apply_upgrade_server_override(
                    config, *server_override, &server_error)) {
                std::cerr << "acecode " << argv[1] << ": " << server_error << "\n"
                          << "usage: acecode " << argv[1]
                          << " [--force] [--server=<url>]\n";
                return 64;
            }
            try {
                save_config(config);
            } catch (const std::exception& e) {
                std::cerr << "acecode " << argv[1]
                          << ": failed to save update server: " << e.what() << "\n";
                return 1;
            }
        }
        return acecode::upgrade::run_upgrade_command(
            config, exe_path, ACECODE_VERSION, std::cout, std::cerr,
            force_update);
    }

    if (argc >= 2 && std::string(argv[1]) == "--apply-update") {
        return acecode::upgrade::run_apply_update_command(
            platform::argv_tail(argc, argv, 2), std::cout, std::cerr,
            acecode::upgrade::current_target());
    }

    if (argc >= 2 && std::string(argv[1]) == "daemon") {
        return acecode::daemon::cli::run(platform::argv_tail(argc, argv, 2), exe_path);
    }

    if (argc >= 2 && std::string(argv[1]) == "service") {
#ifdef _WIN32
        return acecode::daemon::service_win::run_cli(platform::argv_tail(argc, argv, 2),
                                                     exe_path);
#else
        std::cerr << "acecode: native `service` subcommand is Windows-only;\n"
                     "         on Linux/macOS use `acecode daemon --foreground`\n"
                     "         under systemd / launchd (see README for sample units).\n";
        return 65;
#endif
    }

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--service-main") {
#ifdef _WIN32
            return acecode::daemon::service_win::run_service_main_dispatcher();
#else
            std::cerr << "--service-main is Windows-only\n";
            return 64;
#endif
        }
    }

    // ---- -p / --print 无头模式(openspec add-headless-print-mode) ----
    // Windows 的 char** argv 是 ANSI 代码页,中文 prompt 会乱码;用宽字符
    // 命令行重建 UTF-8 token。POSIX 直接用 argv(约定 UTF-8 locale)。
    {
        auto tokens = platform::utf8_command_line_tail(argc, argv);
        if (!tokens.empty() && tokens.front() == "channels") {
            return acecode::channels::run_cli(
                std::vector<std::string>(tokens.begin() + 1, tokens.end()), std::cout, std::cerr);
        }
        if (acecode::headless::should_enter_print_mode(tokens)) {
            auto opts = acecode::headless::parse_headless_cli_options(tokens);
            // --help 优先于用法报错:`-p --help` 后面跟什么都先出帮助。
            if (opts.show_help) {
                std::cout << acecode::headless::print_mode_help();
                return 0;
            }
            if (!opts.error.empty()) {
                std::cerr << "acecode -p: " << opts.error << "\n"
                          << acecode::headless::print_mode_usage_line();
                return 64;
            }
            return acecode::headless::run_print_mode(opts);
        }
    }

#ifdef _WIN32
    if (argc == 1) {
        AppConfig cfg_probe = load_config();
        if (cfg_probe.daemon.auto_start_on_double_click) {
            DWORD procs[2] = {0, 0};
            DWORD n = ::GetConsoleProcessList(procs, 2);
            if (n == 1) {
                std::vector<std::string> tokens = {"start"};
                return acecode::daemon::cli::run(tokens, exe_path);
            }
        }
    }
#endif

    return std::nullopt;
}


} // namespace acecode::cli
