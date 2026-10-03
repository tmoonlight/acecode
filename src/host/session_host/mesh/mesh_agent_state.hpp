#pragma once

// Private state of MeshAgentService, shared by its translation units only.

#include "mesh_agent_service.hpp"

#include <deque>
#include <map>
#include <string>
#include <utility>

namespace acecode::mesh {

struct MeshAgentService::Caller {
    std::string session_id;
    std::string root_id;
    std::string path;           // canonical; "/root" for the root
    std::string project_dir;    // tree index location (shared by the whole tree)
    std::string workspace_cwd;  // session storage cwd used for create / resume
    bool no_workspace = false;
    SessionManager* sm = nullptr;     // Borrowed for one tool call.
    const ToolContext* ctx = nullptr; // Borrowed for one tool call.
};

struct MeshAgentService::Record {
    std::string session_id;     // empty while the spawn is still creating it
    std::string path;
    bool resident = false;      // loaded child counted toward the tree capacity
    bool evicting = false;      // registry destroy in flight
    bool restoring = false;     // create / resume in flight
    bool started = false;       // a turn has started since load
    std::string last_outcome;   // completed | error | aborted
    std::optional<std::string> final_text;
    std::string error_text;
    // Mail for an agent that is not loaded; flushed when it is restored.
    std::deque<std::pair<UserInput, bool>> held_mail;
    ScopedSubscription subscription;
    std::uint64_t last_activity = 0;
};

struct MeshAgentService::Tree {
    std::string root_id;
    std::string project_dir;
    std::string workspace_cwd;
    bool no_workspace = false;
    std::map<std::string, Record> agents; // children by canonical path
    std::size_t pending_slots = 0;
};

// Codex ERROR_MAX_TOKENS (900) at roughly 4 characters per token.
inline constexpr std::size_t kCompletionErrorMaxChars = 3600;
inline constexpr const char* kErrorNextAction =
    "This agent's turn failed. If you still need this agent, use the available collaboration "
    "tools to give it another task.";
inline constexpr const char* kAgentLimitReached = "collab spawn failed: agent thread limit reached";

} // namespace acecode::mesh
