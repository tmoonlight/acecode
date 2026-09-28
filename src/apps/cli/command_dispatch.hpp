#pragma once
#include <optional>
#include <string>
namespace acecode::cli {
std::optional<int> dispatch_non_tui_command(int argc, char* argv[]);
bool is_version_command_arg(const std::string& arg);
bool is_help_command_arg(const std::string& arg);
}
