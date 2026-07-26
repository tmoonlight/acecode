// routes_experts.cpp — local expert component discovery and managed CRUD.
#include "../server_impl.hpp"
#include "../../tool/mcp_manager.hpp"

namespace acecode::web {

using nlohmann::json;

namespace {

const char* mcp_status_name(McpServerState state) {
    switch (state) {
        case McpServerState::Starting: return "starting";
        case McpServerState::Connected: return "connected";
        case McpServerState::Disabled: return "disabled";
        case McpServerState::Failed: return "failed";
        case McpServerState::Cancelled: return "cancelled";
        case McpServerState::TimedOut: return "timed_out";
    }
    return "unavailable";
}

const char* mcp_disabled_reason(McpServerState state) {
    switch (state) {
        case McpServerState::Connected: return "";
        case McpServerState::Disabled: return "globally_disabled";
        case McpServerState::Starting: return "starting";
        case McpServerState::Failed: return "connection_failed";
        case McpServerState::Cancelled: return "connection_cancelled";
        case McpServerState::TimedOut: return "connection_timed_out";
    }
    return "unavailable";
}

const char* mcp_transport_name(McpTransport transport) {
    switch (transport) {
        case McpTransport::Stdio: return "stdio";
        case McpTransport::Sse: return "sse";
        case McpTransport::Http: return "http";
    }
    return "unknown";
}

bool contains_name(const std::optional<std::vector<std::string>>& names,
                   const std::string& name) {
    return !names ||
           std::find(names->begin(), names->end(), name) != names->end();
}

json expert_response_json(const ExpertDefinition& expert,
                          bool include_instructions,
                          const std::string& workspace_hash) {
    json result =
        expert_definition_to_json(expert, include_instructions);
    result["avatar_url"] =
        expert.avatar_path.empty()
            ? std::string{}
            : "/api/experts/" + expert.id +
                  "/avatar?workspace=" + workspace_hash;
    return result;
}

std::optional<std::filesystem::path> safe_avatar_path(
    const ExpertDefinition& expert) {
    if (expert.avatar_path.empty() || expert.package_root.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    const auto root =
        std::filesystem::weakly_canonical(expert.package_root, ec);
    if (ec) return std::nullopt;
    const auto avatar = std::filesystem::weakly_canonical(
        path_from_utf8(expert.avatar_path), ec);
    if (ec || !std::filesystem::is_regular_file(avatar, ec)) {
        return std::nullopt;
    }
    const auto relative = std::filesystem::relative(avatar, root, ec);
    if (ec || relative.empty() || relative == "." ||
        *relative.begin() == "..") {
        return std::nullopt;
    }
    return avatar;
}

bool supported_avatar_mime(const std::string& mime) {
    return mime == "image/png" || mime == "image/jpeg" ||
           mime == "image/gif" || mime == "image/webp" ||
           mime == "image/bmp" || mime == "image/x-icon";
}

} // namespace

void WebServer::Impl::register_experts() {
    CROW_ROUTE(app, "/api/experts").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/experts/capabilities").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/experts/<string>").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&) {
        return cors_preflight(req);
    });
    CROW_ROUTE(app, "/api/experts/<string>/avatar").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&) {
        return cors_preflight(req);
    });

    auto workspace_for_request = [this](const crow::request& req)
        -> std::optional<acecode::desktop::WorkspaceMeta> {
        const char* raw = req.url_params.get("workspace");
        return resolve_workspace(raw && *raw ? std::string(raw) : "__local__");
    };

    CROW_ROUTE(app, "/api/experts/capabilities").methods(crow::HTTPMethod::GET)
    ([this, workspace_for_request](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        const auto workspace = workspace_for_request(req);
        if (!workspace) {
            crow::response response(404);
            response.body = json{{"error", "UNKNOWN_WORKSPACE"},
                                 {"message", "unknown workspace"}}.dump();
            response.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(response));
        }

        std::optional<AppConfig> config_snapshot;
        {
            std::lock_guard<std::mutex> config_lock(app_config_mu);
            if (deps.app_config) config_snapshot = *deps.app_config;
        }

        json skills = json::array();
        if (config_snapshot) {
            const auto payload =
                build_skills_payload(*config_snapshot, workspace->cwd);
            for (const auto& item : payload) {
                const std::string id =
                    item.value("name", std::string{});
                if (id.empty()) continue;
                const bool enabled = item.value("enabled", false);
                const bool globally_allowed =
                    contains_name(config_snapshot->skills.allowed, id);
                const bool available = enabled && globally_allowed;
                const std::string source =
                    item.value("source", std::string{});
                const bool expert_selectable = !source.empty();
                std::string disabled_reason;
                if (!enabled) {
                    disabled_reason = "globally_disabled";
                } else if (!globally_allowed) {
                    disabled_reason = "not_allowed_by_global_policy";
                }
                skills.push_back({
                    {"id", id},
                    {"description",
                     item.value("description", std::string{})},
                    {"source", source},
                    {"available", available},
                    {"globally_enabled", available},
                    {"default_enabled", available},
                    {"expert_selectable", expert_selectable},
                    {"configurable", expert_selectable},
                    {"status", available ? "available" : "disabled"},
                    {"disabled_reason", disabled_reason},
                });
            }
        }

        json mcp_servers = json::array();
        if (deps.mcp_manager) {
            for (const auto& server : deps.mcp_manager->list_servers()) {
                const auto config_it = config_snapshot
                    ? config_snapshot->mcp_servers.find(server.name)
                    : std::map<std::string, McpServerConfig>::const_iterator{};
                const bool configured = !config_snapshot ||
                    config_it != config_snapshot->mcp_servers.end();
                const bool globally_enabled = configured &&
                    (!config_snapshot || !config_it->second.disabled);
                const bool runtime_available =
                    server.state == McpServerState::Connected;
                mcp_servers.push_back({
                    {"id", server.name},
                    {"description", std::string{}},
                    {"transport", server.transport},
                    {"available",
                     globally_enabled && runtime_available},
                    {"globally_enabled", globally_enabled},
                    {"default_enabled", globally_enabled},
                    {"expert_selectable", configured},
                    {"configurable", configured},
                    {"runtime_available", runtime_available},
                    {"status",
                     globally_enabled
                         ? mcp_status_name(server.state)
                         : "disabled"},
                    {"disabled_reason",
                     globally_enabled
                         ? mcp_disabled_reason(server.state)
                         : "globally_disabled"},
                    {"tool_count", server.tool_count},
                });
            }
        } else if (config_snapshot) {
            // Tests and reduced embedders may not own a live manager. Return
            // configured IDs and safe state only; never serialize config
            // command lines, environment, headers, or credentials.
            for (const auto& [id, config] :
                 config_snapshot->mcp_servers) {
                const bool disabled = config.disabled;
                mcp_servers.push_back({
                    {"id", id},
                    {"description", std::string{}},
                    {"transport",
                     mcp_transport_name(config.transport)},
                    {"available", false},
                    {"globally_enabled", !disabled},
                    {"default_enabled", !disabled},
                    {"expert_selectable", true},
                    {"configurable", true},
                    {"runtime_available", false},
                    {"status", disabled ? "disabled" : "unavailable"},
                    {"disabled_reason",
                     disabled ? "globally_disabled"
                              : "runtime_unavailable"},
                    {"tool_count", 0},
                });
            }
        }

        json tools = json::array();
        if (deps.tools) {
            for (const auto& tool : deps.tools->get_registered_tools()) {
                if (tool.source != ToolSource::Builtin) continue;
                tools.push_back({
                    {"id", tool.definition.name},
                    {"description", tool.definition.description},
                    {"available", true},
                    {"globally_enabled", true},
                    {"default_enabled", true},
                    {"expert_selectable", true},
                    {"status", "available"},
                    {"disabled_reason", ""},
                    {"configurable", true},
                    {"read_only", tool.is_read_only},
                });
            }
        }

        crow::response response(200);
        response.body = json{
            {"skills", std::move(skills)},
            {"mcp_servers", std::move(mcp_servers)},
            {"tools", std::move(tools)},
        }.dump();
        response.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(response));
    });

    CROW_ROUTE(app, "/api/experts").methods(crow::HTTPMethod::GET)
    ([this, workspace_for_request](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.expert_registry) return crow::response(503);
        const auto workspace = workspace_for_request(req);
        if (!workspace) {
            crow::response response(404);
            response.body = json{{"error", "UNKNOWN_WORKSPACE"},
                                 {"message", "unknown workspace"}}.dump();
            response.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(response));
        }

        std::vector<ExpertDiagnostic> diagnostics;
        const auto experts = deps.expert_registry->list(workspace->cwd, &diagnostics);
        json items = json::array();
        for (const auto& expert : experts) {
            items.push_back(expert_response_json(
                expert, false, workspace->hash));
        }
        json problems = json::array();
        for (const auto& diagnostic : diagnostics) {
            problems.push_back({{"path", diagnostic.path},
                                {"message", diagnostic.message}});
        }
        crow::response response(200);
        response.body = json{
            {"experts", std::move(items)},
            {"diagnostics", std::move(problems)},
            {"workspace_hash", workspace->hash},
            {"cwd", workspace->cwd},
            {"global_root", path_to_utf8(deps.expert_registry->global_root())},
        }.dump();
        response.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(response));
    });

    CROW_ROUTE(app, "/api/experts/<string>/avatar").methods(crow::HTTPMethod::GET)
    ([this, workspace_for_request](const crow::request& req,
                                  const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.expert_registry) return crow::response(503);
        const auto workspace = workspace_for_request(req);
        if (!workspace) {
            return with_cors(req, crow::response(404));
        }
        const auto expert =
            deps.expert_registry->find(workspace->cwd, id);
        if (!expert) return with_cors(req, crow::response(404));

        const auto avatar = safe_avatar_path(*expert);
        if (!avatar) return with_cors(req, crow::response(404));
        const auto mime = preview_blob_mime(path_to_utf8(*avatar));
        if (!mime || !supported_avatar_mime(*mime)) {
            return with_cors(req, crow::response(404));
        }

        std::error_code ec;
        constexpr std::uintmax_t kMaxAvatarBytes = 8 * 1024 * 1024;
        const auto size = std::filesystem::file_size(*avatar, ec);
        if (ec || size > kMaxAvatarBytes) {
            return with_cors(req, crow::response(404));
        }
        std::ifstream input(*avatar, std::ios::binary);
        if (!input) return with_cors(req, crow::response(404));
        std::string bytes(static_cast<std::size_t>(size), '\0');
        if (size > 0) {
            input.read(bytes.data(), static_cast<std::streamsize>(size));
            if (!input) return with_cors(req, crow::response(404));
        }

        crow::response response(std::move(bytes));
        response.add_header("Content-Type", *mime);
        response.add_header("Cache-Control", "private, max-age=300");
        response.add_header("X-Content-Type-Options", "nosniff");
        return with_cors(req, std::move(response));
    });

    CROW_ROUTE(app, "/api/experts/<string>").methods(crow::HTTPMethod::GET)
    ([this, workspace_for_request](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.expert_registry) return crow::response(503);
        const auto workspace = workspace_for_request(req);
        if (!workspace) {
            crow::response response(404);
            response.body = json{{"error", "UNKNOWN_WORKSPACE"}}.dump();
            response.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(response));
        }
        auto expert = deps.expert_registry->find(workspace->cwd, id);
        if (!expert) {
            crow::response response(404);
            response.body = json{{"error", "EXPERT_NOT_FOUND"},
                                 {"message", "expert component not found"}}.dump();
            response.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(response));
        }
        crow::response response(
            expert_response_json(*expert, true, workspace->hash).dump());
        response.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(response));
    });

    CROW_ROUTE(app, "/api/experts").methods(crow::HTTPMethod::POST)
    ([this, workspace_for_request](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.expert_registry) return crow::response(503);
        auto error_response = [&](int status, const char* code, const std::string& message) {
            crow::response response(status);
            response.body = json{{"error", code}, {"message", message}}.dump();
            response.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(response));
        };
        const auto workspace = workspace_for_request(req);
        if (!workspace) return error_response(404, "UNKNOWN_WORKSPACE", "unknown workspace");
        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception& ex) {
            return error_response(400, "BAD_JSON", ex.what());
        }
        std::string error;
        auto draft = ExpertRegistry::draft_from_json(body, &error);
        if (!draft) return error_response(400, "INVALID_EXPERT", error);
        if (const auto effective =
                deps.expert_registry->find(workspace->cwd, draft->id)) {
            if (!effective->managed_global) {
                return error_response(
                    409, "WORKSPACE_EXPERT_READ_ONLY",
                    "workspace expert packages are read-only through this API");
            }
            return error_response(
                409, "EXPERT_ALREADY_EXISTS",
                "a global expert with this ID already exists");
        }
        if (!deps.expert_registry->create_global(*draft, &error, workspace->cwd)) {
            if (error.find("already exists") != std::string::npos) {
                return error_response(409, "EXPERT_ALREADY_EXISTS", error);
            }
            return error_response(400, "CREATE_FAILED", error);
        }
        auto created = deps.expert_registry->find(workspace->cwd, draft->id);
        crow::response response(201);
        response.body = created
            ? expert_response_json(*created, true, workspace->hash).dump()
            : json{{"id", draft->id}, {"ok", true}}.dump();
        response.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(response));
    });

    CROW_ROUTE(app, "/api/experts/<string>").methods(crow::HTTPMethod::PUT)
    ([this, workspace_for_request](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.expert_registry) return crow::response(503);
        auto error_response = [&](int status, const char* code, const std::string& message) {
            crow::response response(status);
            response.body = json{{"error", code}, {"message", message}}.dump();
            response.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(response));
        };
        const auto workspace = workspace_for_request(req);
        if (!workspace) return error_response(404, "UNKNOWN_WORKSPACE", "unknown workspace");
        const auto effective = deps.expert_registry->find(workspace->cwd, id);
        if (effective && !effective->managed_global) {
            return error_response(409, "WORKSPACE_EXPERT_READ_ONLY",
                                  "workspace expert packages are read-only through this API");
        }
        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception& ex) {
            return error_response(400, "BAD_JSON", ex.what());
        }
        if (!body.is_object()) return error_response(400, "BAD_JSON", "body must be an object");
        if (!body.contains("id") && !body.contains("name")) body["id"] = id;
        std::string error;
        auto draft = ExpertRegistry::draft_from_json(body, &error);
        if (!draft) return error_response(400, "INVALID_EXPERT", error);
        if (!deps.expert_registry->update_global(id, *draft, &error, workspace->cwd)) {
            return error_response(error.find("does not exist") != std::string::npos ? 404 : 400,
                                  "UPDATE_FAILED", error);
        }
        auto updated = deps.expert_registry->find(workspace->cwd, id);
        crow::response response(updated
            ? expert_response_json(*updated, true, workspace->hash).dump()
            : json{{"id", id}, {"ok", true}}.dump());
        response.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(response));
    });

    CROW_ROUTE(app, "/api/experts/<string>").methods(crow::HTTPMethod::Delete)
    ([this, workspace_for_request](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.expert_registry) return crow::response(503);
        auto error_response = [&](int status, const char* code, const std::string& message) {
            crow::response response(status);
            response.body = json{{"error", code}, {"message", message}}.dump();
            response.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(response));
        };
        const auto workspace = workspace_for_request(req);
        if (!workspace) return error_response(404, "UNKNOWN_WORKSPACE", "unknown workspace");
        const auto effective = deps.expert_registry->find(workspace->cwd, id);
        if (effective && !effective->managed_global) {
            return error_response(409, "WORKSPACE_EXPERT_READ_ONLY",
                                  "workspace expert packages are read-only through this API");
        }
        std::string error;
        if (!deps.expert_registry->delete_global(id, &error)) {
            return error_response(404, "DELETE_FAILED", error);
        }
        crow::response response(json{{"ok", true}, {"id", id}}.dump());
        response.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(response));
    });
}

} // namespace acecode::web
