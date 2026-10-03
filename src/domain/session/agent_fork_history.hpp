#pragma once

// fork_turns for agent_spawn (Codex SpawnAgentArgs::fork_mode and
// keep_forked_rollout_item). The input is the parent's effective model history
// (reconstruct_effective_model_history: compaction replacement + suffix). The
// child keeps user messages, compaction summaries and assistant final answers;
// tool calls/results, reasoning, internal context and inter-agent envelopes
// are dropped.

#include "llm/llm_provider.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace acecode::mesh {

struct ForkTurns {
    enum class Kind { None, All, LastN };
    Kind kind = Kind::All;
    std::size_t last_n = 0;
};

// Empty/whitespace -> all; "none" / "all" (case-insensitive); positive integer
// string -> last N user turns. Anything else -> nullopt with the Codex error.
std::optional<ForkTurns> parse_fork_turns(const std::string& text, std::string* error = nullptr);

// Inherited messages carry metadata.mesh_inherited = true.
std::vector<ChatMessage> build_fork_history(const std::vector<ChatMessage>& effective_history,
                                            const ForkTurns& fork);

} // namespace acecode::mesh
