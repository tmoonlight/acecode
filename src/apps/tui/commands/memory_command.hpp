#pragma once

#include "command_registry.hpp"

namespace acecode {

// Register the `/memory` slash command. The text comes from the shared
// dispatch_memory_command (same output as the web/desktop /memory):
//   list [--scope=global|workspace] [--type=<t>] / view <name> / edit <name>
//   forget <name> / flush / off / on / reload
// Only `edit` is TUI-specific: it opens the entry in $EDITOR.
void register_memory_command(CommandRegistry& registry);

} // namespace acecode
