#include "tui/model/mcp_sidebar_model.hpp"
#include <utility>

namespace acecode::tui {
std::string mcp_state_label(McpServerState state) {
    switch (state) {
        case McpServerState::Starting:  return "starting";
        case McpServerState::Connected: return "connected";
        case McpServerState::Disabled:  return "disabled";
        case McpServerState::Failed:    return "failed";
        case McpServerState::Cancelled: return "cancelled";
        case McpServerState::TimedOut:  return "timed_out";
    }
    return "unknown";
}

bool mcp_sidebar_has_loading(
    const std::vector<TuiState::McpSidebarServer>& servers) {
    for (const auto& server : servers) {
        if (server.state == "starting") return true;
    }
    return false;
}

bool mcp_sidebar_has_loading(const TuiState& state) {
    return mcp_sidebar_has_loading(state.mcp_sidebar_servers);
}

std::vector<TuiState::McpSidebarServer>
build_mcp_sidebar_servers(const McpManager& manager) {
    auto server_infos = manager.list_servers();
    std::vector<TuiState::McpSidebarServer> out;
    out.reserve(server_infos.size());
    for (const auto& info : server_infos) {
        TuiState::McpSidebarServer server;
        server.name = info.name;
        server.state = mcp_state_label(info.state);
        server.transport = info.transport;
        server.error = info.error;
        server.tool_count = info.tool_count;
        out.push_back(std::move(server));
    }
    return out;
}

void set_mcp_sidebar_servers_locked(
    TuiState& state,
    std::vector<TuiState::McpSidebarServer> servers) {
    state.mcp_sidebar_servers = std::move(servers);
}


} // namespace acecode::tui
