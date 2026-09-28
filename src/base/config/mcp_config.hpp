#pragma once

#include "config.hpp"

#include <optional>
#include <stdexcept>

namespace acecode {

using McpServerMap = std::map<std::string, McpServerConfig>;

// This is the client configuration schema, not the MCP wire-message schema.
const nlohmann::json& mcp_config_schema();
nlohmann::json mcp_config_document_schema();
nlohmann::json validate_mcp_config(const nlohmann::json& servers);

class McpConfigError : public std::runtime_error {
public:
    explicit McpConfigError(nlohmann::json errors,
                           bool document = false);
    const nlohmann::json& payload() const { return payload_; }
private:
    nlohmann::json payload_;
};

void require_valid_mcp_config(const nlohmann::json& servers);
McpServerMap parse_mcp_config(const nlohmann::json& servers,
                            const McpServerMap* preserve_hidden = nullptr);
nlohmann::json serialize_mcp_config(const McpServerMap& servers,
                                    bool include_auth_token = true);

std::string mcp_project_root(const std::string& cwd);
std::string mcp_project_config_path(const std::string& cwd);
std::string mcp_project_server_id(const std::string& cwd,
                                 const std::string& name);
std::string mcp_server_display_name(const std::string& id);
McpServerMap load_project_mcp_config(const std::string& cwd);
void save_project_mcp_config(const std::string& cwd,
                             const nlohmann::json& servers);
McpServerMap effective_mcp_config(const McpServerMap& global,
                                const McpServerMap& project);

// A separate MCP snapshot avoids rolling unrelated global settings back.
std::string mcp_last_good_path(const std::string& config_path);
void remember_valid_mcp_config(const std::string& config_path,
                               const nlohmann::json& servers);
// Called on a parsed global document before parsing its MCP section. Returns
// true if only the MCP section was restored and the active file rewritten.
bool recover_mcp_config(nlohmann::json& document,
                        const std::string& config_path);
void write_validated_config_file(const std::string& config_path,
                                 const std::string& bytes,
                                 const nlohmann::json& servers);

// Common text editors use this before touching a known MCP config file.
// nullopt means an unrelated file; invalid JSON/fields throw McpConfigError.
std::optional<nlohmann::json> validate_mcp_file_edit(
    const std::string& path, const std::string& content);

} // namespace acecode
