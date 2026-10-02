#pragma once

// Mesh swarm (Codex Multi-Agent V2) role and mode instructions, ported from
// codex-rs/prompts/src/multi_agent_instructions.rs and
// model_messages/multi_agent.rs with ACECode tool names. The output depends
// only on these options, so it is byte-stable within a turn and identical for
// every child agent of a tree (the agent path itself arrives in NEW_TASK).

#include <string>

namespace acecode {

struct MeshSwarmPromptOptions {
    bool is_root = true;
    int max_concurrency = 4;
    // agent_wait is visible to the model.
    bool wait_agent_enabled = true;
    // agent_spawn exposes model / reasoning_effort overrides.
    bool expose_model_overrides = true;
};

// Role text (root or child) + shared guidance + concurrency slots, then the
// proactive multi-agent mode text. Wrapped in <multi_agent_role> and
// <multi_agent_mode> tags because ACECode injects request context as a
// user-role message.
std::string build_mesh_swarm_context_prompt(const MeshSwarmPromptOptions& options);

} // namespace acecode
