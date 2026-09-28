#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace acecode::web {

// Builds the secret-free WebSocket notification emitted after a remote-control
// session switch. Keeping the payload construction separate makes the public
// wire contract directly testable without a live Crow connection.
nlohmann::json remote_control_session_selected_event_json(
    const std::string& session_id,
    const std::string& workspace_hash,
    const std::string& cwd,
    bool no_workspace,
    const std::string& title,
    const std::string& updated_at);

} // namespace acecode::web
