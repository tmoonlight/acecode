#include "cli/process_environment.hpp"
#include "cli/command_dispatch.hpp"
#include "cli/pre_tui_commands.hpp"
#include "cli/interactive_options.hpp"
#include "config/mcp_config.hpp"
#include "tui/app/tui_app.hpp"
#include <iostream>

int main(int argc, char* argv[]) try {
    acecode::cli::configure_process_environment();
    if (auto exit_code = acecode::cli::dispatch_non_tui_command(argc, argv)) return *exit_code;
    auto argv0_dir = acecode::cli::get_executable_dir_from_argv(argc, argv);
    auto cli = acecode::parse_interactive_cli_options(argc, argv);
    if (auto exit_code = acecode::cli::run_pre_tui_command(cli, argv0_dir)) return *exit_code;
    return acecode::tui::TuiApp({std::move(cli), std::move(argv0_dir)}).run();
} catch (const acecode::McpConfigError& error) {
    std::cerr << error.what() << std::endl;
    return 1;
}
