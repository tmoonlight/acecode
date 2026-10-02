#pragma once

// Session-level swarm mode. Star is the original spawn_subagent fan-out;
// mesh is the Codex Multi-Agent V2 clone with agent_* tools. The two tool
// families are mutually exclusive: each mode hides the other family.

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace acecode {

enum class SwarmMode { Off, Star, Mesh };

// Canonical names: "off", "star", "mesh".
const char* swarm_mode_name(SwarmMode mode);
// Accepts canonical names (case-insensitive) and the legacy boolean words
// "true" (= star) and "false" (= off). Empty or unknown input -> nullopt.
std::optional<SwarmMode> parse_swarm_mode(const std::string& text);

// agent_spawn, agent_list, agent_send_message, agent_followup_task,
// agent_wait, agent_interrupt.
const std::vector<std::string>& mesh_agent_tool_names();
// Built-in tools hidden in `mode`, mapped to the model-facing refusal text.
// Off/star hide the mesh tools; mesh hides spawn/wait_subagent and the thread
// tools (they would offer a second, conflicting way to message sessions).
std::unordered_map<std::string, std::string> swarm_mode_hidden_tools(SwarmMode mode);

} // namespace acecode
