#pragma once
#include "state.hpp"
#include <iosfwd>
#include <vector>
namespace acecode::channels {
Json parse_command(const std::string& arguments);
std::string format_result(const Json& result);
std::string command_help();
int run_cli(const std::vector<std::string>& arguments, std::ostream& out, std::ostream& error);
} // namespace acecode::channels
