#pragma once
#include "tui/tui_state.hpp"
#include "tool/mcp_manager.hpp"

namespace acecode::tui {
std::string mcp_state_label(McpServerState state);
bool mcp_sidebar_has_loading(const std::vector<TuiState::McpSidebarServer>& servers);
bool mcp_sidebar_has_loading(const TuiState& state);
std::vector<TuiState::McpSidebarServer> build_mcp_sidebar_servers(const McpManager& manager);
void set_mcp_sidebar_servers_locked(TuiState& state, std::vector<TuiState::McpSidebarServer> servers);
} // namespace acecode::tui
