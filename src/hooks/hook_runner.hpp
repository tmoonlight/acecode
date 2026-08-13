#pragma once

#include "hook_config.hpp"

#include <chrono>
#include <map>
#include <string>

namespace acecode {

using HookEnvironment = std::map<std::string, std::string>;

struct HookProcessResult {
    bool started = false;
    bool timed_out = false;
    int exit_code = -1;
    long long duration_ms = 0;
    std::string stdout_text;
    std::string stderr_text;
    // Legacy combined output for existing diagnostics.
    std::string output;
    // Process start / runner errors, not child stderr.
    std::string error;
};

std::string resolve_hook_command_path(const std::string& command);

HookProcessResult run_hook_process(const HookCommandSpec& command,
                                   const std::string& stdin_text,
                                   int timeout_ms,
                                   const std::string& cwd);

HookProcessResult run_hook_shell_command(const std::string& command,
                                         const std::string& stdin_text,
                                         int timeout_ms,
                                         const std::string& cwd,
                                         const HookEnvironment& environment = {});

} // namespace acecode
