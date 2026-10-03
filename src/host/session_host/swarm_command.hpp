#pragma once

// /swarm [star|mesh|off] — the single text implementation shared by the TUI
// builtin command and the daemon builtin whitelist.

#include "session/swarm_mode.hpp"

#include <optional>
#include <string>

namespace acecode {

struct SwarmCommandRequest {
    // Empty args: show the current mode.
    bool show = false;
    std::optional<SwarmMode> mode;
    // Non-empty: invalid argument, reported verbatim.
    std::string error;
};

SwarmCommandRequest parse_swarm_command(const std::string& args);
std::string swarm_command_status_text(SwarmMode current);
std::string swarm_command_applied_text(SwarmMode mode);

} // namespace acecode
