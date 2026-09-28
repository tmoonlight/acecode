#pragma once

#include "mcp_manager.hpp"

namespace acecode {

// Resolves public server selections to exact global/project runtime owners.
// Empty cwd deliberately opts out of project discovery (headless allowlists).
ToolCapabilityPolicy mcp_scope_policy(
    const AppConfig* config,
    const std::string& cwd,
    const std::optional<std::vector<std::string>>& selected_servers = std::nullopt,
    McpManager* manager = nullptr,
    ToolExecutor* tools = nullptr);

} // namespace acecode
