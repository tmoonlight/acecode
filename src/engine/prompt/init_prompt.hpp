#pragma once

#include <filesystem>
#include <string>

namespace acecode {

// Generate the AGENTS.md skeleton text for a directory. The hint block at the
// top is included only when a legacy `CLAUDE.md` already lives in the same
// directory. Pure function — unit tested directly. Used by the no-provider
// fallback path of `/init`.
std::string build_agent_md_skeleton(const std::filesystem::path& cwd);

// Build the LLM prompt that `/init` submits when a provider is configured. The
// returned string is the full user-role message sent to the agent loop; the
// LLM uses its read tools to survey the codebase and writes / edits AGENTS.md
// via file_write_tool / file_edit_tool. Pure function — unit tested directly.
// The prompt body is stable; only the suffix varies based on whether
// AGENTS.md / CLAUDE.md already exist in `cwd`.
std::string build_init_prompt(const std::filesystem::path& cwd);

} // namespace acecode
