#include "remote_control_session_event.hpp"

#include <nlohmann/json.hpp>

#include <chrono>

namespace acecode::web {

nlohmann::json remote_control_session_selected_event_json(
    const std::string& session_id,
    const std::string& workspace_hash,
    const std::string& cwd,
    bool no_workspace,
    const std::string& title,
    const std::string& updated_at) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return {
        {"type", "remote_control_session_selected"},
        {"timestamp_ms", now},
        {"payload", {
            {"session_id", session_id},
            {"workspace_hash", workspace_hash},
            {"cwd", cwd},
            {"no_workspace", no_workspace},
            {"title", title},
            {"updated_at", updated_at},
            {"remote_control_bound", true},
        }},
    };
}

} // namespace acecode::web
