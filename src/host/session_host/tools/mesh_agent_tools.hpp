#pragma once

// agent_spawn / agent_list / agent_send_message / agent_followup_task /
// agent_wait / agent_interrupt — the mesh swarm tool family (Codex
// Multi-Agent V2 spawn_agent / list_agents / send_message / followup_task /
// wait_agent / interrupt_agent with ACECode's agent_ prefix). Visible only in
// mesh swarm mode (session/swarm_mode.hpp). Closures capture weak_ptr<Service>
// and are registered after the SessionRegistry exists (ownership rule C12).

#include "tool/tool_executor.hpp"

#include <memory>

namespace acecode {

struct AppConfig;
namespace mesh { class MeshAgentService; }

// Descriptions embed swarm.mesh (wait bounds, model-override exposure) and the
// saved model names of `config` at registration time.
void register_mesh_agent_tools(ToolExecutor& tools,
                               std::weak_ptr<mesh::MeshAgentService> service,
                               const AppConfig& config);
// Headless registers the tools before its SessionRegistry exists (so
// --list-tools / --disable-tools see them) and binds the live service once the
// registry is built. Tools removed by --disable-tools stay removed.
void rebind_mesh_agent_tools(ToolExecutor& tools,
                             std::weak_ptr<mesh::MeshAgentService> service,
                             const AppConfig& config);

} // namespace acecode
