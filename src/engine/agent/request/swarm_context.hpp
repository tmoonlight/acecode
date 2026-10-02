#pragma once

#include "prompt/mesh_swarm_prompts.hpp"
#include "session/swarm_mode.hpp"

#include <string>

namespace acecode::agent {

// Session swarm mode plus the mesh identity the request builder needs. The
// session host publishes it; each turn captures one value with the rest of
// RequestContextSource, so a mode switch never changes a running turn.
struct SwarmContext {
    SwarmMode mode = SwarmMode::Off;
    // Canonical mesh path; "/root" for a mesh root, empty when not in mesh.
    std::string agent_path;
    MeshSwarmPromptOptions mesh;
};

} // namespace acecode::agent
