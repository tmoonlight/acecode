#include "web/server_impl.hpp"
#include "config/mcp_config.hpp"
#include "tool/mcp_manager.hpp"
#include "utils/utf8_path.hpp"

namespace acecode::web {
using nlohmann::json;
namespace {
crow::response mcp_reply(int status, const json& body) {
    crow::response response(status, body.dump());
    response.add_header("Content-Type", "application/json");
    response.add_header("Cache-Control", "no-store");
    return response;
}
McpConfigError bad_mcp_input(const std::string& path, const std::string& message) {
    return McpConfigError(json::array({{{"path", path}, {"message", message}}}));
}
json mcp_request_json(const crow::request& req) {
    try { return json::parse(req.body); }
    catch (const json::exception&) { throw bad_mcp_input("", "invalid JSON document"); }
}
} // namespace

void WebServer::Impl::register_mcp() {
    // A workspace hash resolves through the registered workspace catalog;
    // clients cannot use this endpoint to write an arbitrary filesystem path.
    const auto scope = [this](const crow::request& req) -> std::optional<std::string> {
        const char* hash = req.url_params.get("workspace");
        if (!hash || !*hash) return std::string{};
        const auto workspace = resolve_workspace(hash);
        if (!workspace) return std::nullopt;
        return workspace->cwd;
    };
    const auto apply_runtime = [this](const std::string& cwd, const McpServerMap& servers,
                                      const AppConfig& snapshot) {
        const bool applied = deps.mcp_manager && deps.tools;
        if (applied) {
            std::unordered_set<std::string> keep_enabled;
            if (deps.session_registry) {
                for (const auto& [name, server] : servers) {
                    if (server.disabled && deps.session_registry->expert_requires_mcp_server(name, cwd))
                        keep_enabled.insert(name);
                }
            }
            deps.mcp_manager->reconcile_scope(cwd, servers, *deps.tools, keep_enabled);
        }
        if (deps.session_registry) deps.session_registry->refresh_mcp_policy(snapshot);
        return applied;
    };

    CROW_ROUTE(app, "/api/mcp/schema").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        return mcp_reply(200, {{"schema", mcp_config_schema()},
                              {"document_schema", mcp_config_document_schema()}});
    });

    CROW_ROUTE(app, "/api/mcp").methods(crow::HTTPMethod::GET)
    ([this, scope](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        const auto cwd = scope(req);
        if (!cwd) return mcp_reply(404, {{"error", "workspace not found"}});
        try {
            std::shared_lock<std::shared_mutex> lock(app_config_mu);
            const auto servers = cwd->empty()
                ? (deps.app_config ? deps.app_config->mcp_servers : McpServerMap{})
                : load_project_mcp_config(*cwd);
            return mcp_reply(200, serialize_mcp_config(servers, false));
        } catch (const McpConfigError& error) {
            return mcp_reply(400, error.payload());
        } catch (const std::exception&) {
            return mcp_reply(500, {{"error", "MCP_CONFIG_READ_FAILED"}});
        }
    });

    CROW_ROUTE(app, "/api/mcp").methods(crow::HTTPMethod::PUT)
    ([this, scope, apply_runtime](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        std::lock_guard<std::mutex> operation_lock(mcp_config_mu);
        if (!deps.app_config) return mcp_reply(503, {{"error", "configuration unavailable"}});
        const auto cwd = scope(req);
        if (!cwd) return mcp_reply(404, {{"error", "workspace not found"}});
        try {
            const auto raw = mcp_request_json(req);
            require_valid_mcp_config(raw);
            McpServerMap servers;
            AppConfig snapshot;
            {
                std::lock_guard<std::shared_mutex> lock(app_config_mu);
                McpServerMap previous;
                if (cwd->empty()) previous = deps.app_config->mcp_servers;
                else {
                    // A valid full replacement can repair a project file
                    // even when no valid previous snapshot exists.
                    try { previous = load_project_mcp_config(*cwd); }
                    catch (const McpConfigError&) { }
                }
                servers = parse_mcp_config(raw, &previous);
                snapshot = *deps.app_config;
                if (cwd->empty()) {
                    snapshot.mcp_servers = servers;
                    if (deps.config_path.empty()) save_config(snapshot);
                    else save_config(snapshot, deps.config_path);
                    deps.app_config->mcp_servers = servers;
                } else {
                    save_project_mcp_config(*cwd, serialize_mcp_config(servers));
                }
            }
            const bool applied = apply_runtime(*cwd, servers, snapshot);
            return mcp_reply(200, {{"saved", true}, {"reload_required", !applied}, {"applied", applied}});
        } catch (const McpConfigError& error) {
            return mcp_reply(400, error.payload());
        } catch (const std::exception&) {
            return mcp_reply(500, {{"error", "MCP_CONFIG_SAVE_FAILED"},
                                  {"message", "Could not persist MCP configuration"}});
        }
    });

    CROW_ROUTE(app, "/api/mcp/toggle").methods(crow::HTTPMethod::POST)
    ([this, scope, apply_runtime](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        std::lock_guard<std::mutex> operation_lock(mcp_config_mu);
        if (!deps.app_config) return mcp_reply(503, {{"error", "configuration unavailable"}});
        const auto cwd = scope(req);
        if (!cwd) return mcp_reply(404, {{"error", "workspace not found"}});
        try {
            const auto request = mcp_request_json(req);
            if (!request.is_object() || !request.contains("name") || !request["name"].is_string())
                throw bad_mcp_input("/name", "server name is required");
            if (!request.contains("enabled") || !request["enabled"].is_boolean())
                throw bad_mcp_input("/enabled", "must be boolean");
            const auto name = request["name"].get<std::string>();
            const bool enabled = request["enabled"].get<bool>();
            McpServerMap servers;
            AppConfig snapshot;
            {
                std::lock_guard<std::shared_mutex> lock(app_config_mu);
                servers = cwd->empty() ? deps.app_config->mcp_servers : load_project_mcp_config(*cwd);
                const auto found = servers.find(name);
                if (found == servers.end()) return mcp_reply(404, {{"error", "unknown mcp server"}});
                found->second.disabled = !enabled;
                require_valid_mcp_config(serialize_mcp_config(servers));
                snapshot = *deps.app_config;
                if (cwd->empty()) {
                    snapshot.mcp_servers = servers;
                    if (deps.config_path.empty()) save_config(snapshot);
                    else save_config(snapshot, deps.config_path);
                    deps.app_config->mcp_servers = servers;
                } else {
                    save_project_mcp_config(*cwd, serialize_mcp_config(servers));
                }
            }
            const bool retained = !enabled && deps.session_registry &&
                deps.session_registry->expert_requires_mcp_server(name, *cwd);
            return mcp_reply(200, {{"name", name}, {"enabled", enabled},
                {"applied", apply_runtime(*cwd, servers, snapshot)}, {"retained_for_expert", retained}});
        } catch (const McpConfigError& error) {
            return mcp_reply(400, error.payload());
        } catch (const std::exception&) {
            return mcp_reply(500, {{"error", "MCP_CONFIG_SAVE_FAILED"}});
        }
    });

    CROW_ROUTE(app, "/api/mcp/reload").methods(crow::HTTPMethod::POST)
    ([this, scope, apply_runtime](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        std::lock_guard<std::mutex> operation_lock(mcp_config_mu);
        if (!deps.app_config || !deps.mcp_manager || !deps.tools)
            return mcp_reply(503, {{"error", "MCP runtime unavailable"}});
        const auto cwd = scope(req);
        if (!cwd) return mcp_reply(404, {{"error", "workspace not found"}});
        try {
            McpServerMap servers;
            AppConfig snapshot;
            {
                std::lock_guard<std::shared_mutex> lock(app_config_mu);
                snapshot = *deps.app_config;
                if (cwd->empty()) {
                    const auto path = deps.config_path.empty()
                        ? path_to_utf8(path_from_utf8(get_acecode_dir()) / "config.json") : deps.config_path;
                    servers = load_config_from_path(path).mcp_servers;
                    snapshot.mcp_servers = servers;
                    deps.app_config->mcp_servers = servers;
                } else {
                    servers = load_project_mcp_config(*cwd);
                }
            }
            return mcp_reply(200, {{"reloaded", apply_runtime(*cwd, servers, snapshot)}});
        } catch (const McpConfigError& error) {
            return mcp_reply(400, error.payload());
        } catch (const std::exception&) {
            return mcp_reply(500, {{"error", "MCP_CONFIG_RELOAD_FAILED"}});
        }
    });
}
} // namespace acecode::web
