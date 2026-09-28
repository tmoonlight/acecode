#pragma once
#include <iosfwd>
#include <optional>
#include <string>
namespace acecode::upgrade {
// Parses only; configuration and network side effects remain in CLI dispatch.
std::optional<int> parse_upgrade_cli_args(int argc, char* argv[], bool& force_update,
    std::optional<std::string>& server_override, std::ostream& error);
}
