#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace acecode {

// TUI slash-command adaptive ordering. The persisted shape is:
//   { "tui_slash_command_usage": { "help": 3, "model": 7 } }
// Reads are tolerant: missing/malformed containers and invalid individual
// entries are ignored. Only aggregate command names/counts are stored.
std::map<std::string, std::uint64_t> read_tui_slash_command_usage();

struct SlashCommandUsageWriteResult {
    // Updated count for the current process even when persistence failed.
    std::uint64_t count = 0;
    bool persisted = false;
};

// Atomically increment one command's durable count while preserving unrelated
// state.json keys. Counts saturate at uint64_t max rather than overflowing.
SlashCommandUsageWriteResult record_tui_slash_command_use(
    const std::string& command_name);

} // namespace acecode
