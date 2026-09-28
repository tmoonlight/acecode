#pragma once

#include "command_registry.hpp"

namespace acecode {

bool has_usable_init_provider(const AppConfig& cfg);

// Register `/init` — generate or improve AGENT.md in cwd. With a configured
// provider `/init` delegates the analysis to the LLM. With no provider it falls
// back to writing the static skeleton produced by `build_agent_md_skeleton`.
void register_init_command(CommandRegistry& registry);

} // namespace acecode
