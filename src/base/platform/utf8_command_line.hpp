#pragma once
#include <string>
#include <vector>
namespace acecode::platform {
std::vector<std::string> argv_tail(int argc, char* argv[], int start);
// Optional Windows command line is an input boundary for deterministic tests.
// Null uses GetCommandLineW; POSIX always consumes the original UTF-8 argv.
std::vector<std::string> utf8_command_line_tail(int argc, char* argv[],
    const wchar_t* windows_command_line = nullptr);
}
