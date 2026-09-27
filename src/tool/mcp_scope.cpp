#include "mcp_scope.hpp"
#include "config/mcp_config.hpp"

namespace acecode {
ToolCapabilityPolicy mcp_scope_policy(
    const AppConfig* config,
    const std::string& cwd,
    const std::optional<std::vector<std::string>>& selected_servers,
    McpManager* manager,
    ToolExecutor* tools) {
    ToolCapabilityPolicy policy;
    if (!config && !selected_servers) return policy;
    const auto project = cwd.empty() ? McpServerMap{} : load_project_mcp_config(cwd);
    if (manager && tools && !cwd.empty()) manager->reconcile_scope(cwd, project, *tools);
    const auto effective = effective_mcp_config(config ? config->mcp_servers : McpServerMap{}, project);
    std::unordered_set<std::string> allowed;
    const auto owner = [&](const std::string& name) {
        return project.count(name) ? mcp_project_server_id(cwd, name) : name;
    };
    if (selected_servers) {
        for (const auto& name : *selected_servers) {
            // Unknown public names may be provided by registered tool owners;
            // keep expert selections compatible, but never accept an internal
            // project owner as a user-facing server name.
            if (name.empty() || name.find('/') != std::string::npos) continue;
            const auto id = owner(name);
            allowed.insert(id);
            if (manager && tools) (void)manager->enable(id, *tools);
        }
    } else {
        for (const auto& [name, server] : effective) {
            if (!server.disabled) allowed.insert(owner(name));
        }
    }
    policy.mcp_servers = std::move(allowed);
    return policy;
}
} // namespace acecode
