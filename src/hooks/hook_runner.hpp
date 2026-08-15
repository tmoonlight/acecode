#pragma once

#include "hook_config.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <string>

namespace acecode {

using HookEnvironment = std::map<std::string, std::string>;

struct HookProcessResult {
    bool started = false;
    bool timed_out = false;
    bool aborted = false;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    bool output_limit_reached = false;
    int exit_code = -1;
    long long duration_ms = 0;
    std::string stdout_text;
    std::string stderr_text;
    // Legacy combined output for existing diagnostics.
    std::string output;
    // Process start / runner errors, not child stderr.
    std::string error;
};

// Optional controls for callers that need a cancellable, bounded subprocess.
// The legacy run_hook_process overload below preserves the original 64 KiB
// capture and direct-process timeout behavior.
struct HookProcessOptions {
    int timeout_ms = 0;
    const std::atomic<bool>* abort_flag = nullptr;
    std::size_t max_stdout_bytes = 64 * 1024;
    std::size_t max_stderr_bytes = 64 * 1024;
    // 0 means unlimited. The newline that fills the budget is retained.
    std::size_t max_stdout_lines = 0;
    bool terminate_on_stdout_limit = false;
    bool terminate_process_tree = false;
    bool append_output_truncation_notice = true;
};

std::string resolve_hook_command_path(const std::string& command);

HookProcessResult run_hook_process(const HookCommandSpec& command,
                                   const std::string& stdin_text,
                                   int timeout_ms,
                                   const std::string& cwd);

HookProcessResult run_hook_process(const HookCommandSpec& command,
                                   const std::string& stdin_text,
                                   const std::string& cwd,
                                   const HookProcessOptions& options);

HookProcessResult run_hook_shell_command(const std::string& command,
                                         const std::string& stdin_text,
                                         int timeout_ms,
                                         const std::string& cwd,
                                         const HookEnvironment& environment = {});

} // namespace acecode
