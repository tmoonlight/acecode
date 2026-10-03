#pragma once

#include "config/config.hpp"
#include "prompt/mesh_swarm_prompts.hpp"
#include "session/swarm_mode.hpp"

#include <string>

namespace acecode::agent {

// Session swarm mode plus the mesh identity the request builder needs. A turn
// captures one value with the rest of RequestContextSource, so a mode switch
// never changes a running turn. When the loop has a SessionManager, mode and
// agent path are read from it at capture (session metadata is the source of
// truth); the published value then only supplies the mesh prompt options.
struct SwarmContext {
    SwarmMode mode = SwarmMode::Off;
    // Canonical mesh path; "/root" for a mesh root, empty when not in mesh.
    std::string agent_path;
    MeshSwarmPromptOptions mesh;
};

inline SwarmContext make_swarm_context(SwarmMode mode, const std::string& agent_path,
                                       const MeshSwarmConfig& config) {
    SwarmContext context;
    context.mode = mode;
    context.mesh.max_concurrency = config.max_concurrent_agents;
    context.mesh.expose_model_overrides = config.expose_model_overrides;
    if (mode == SwarmMode::Mesh) {
        context.agent_path = agent_path.empty() ? std::string("/root") : agent_path;
        context.mesh.is_root = context.agent_path == "/root";
    }
    return context;
}

} // namespace acecode::agent
