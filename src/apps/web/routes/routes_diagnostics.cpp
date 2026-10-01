#include "web/server_impl.hpp"
#include "session/session_data_diagnostics.hpp"
#include "session/session_load_metrics.hpp"

#include <cmath>

namespace acecode::web {

void WebServer::Impl::register_session_diagnostics() {
    CROW_ROUTE(app, "/api/diagnostics/sessions").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/diagnostics/session-open").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/diagnostics/sessions").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        std::vector<desktop::WorkspaceMeta> workspaces;
        if (deps.workspace_registry) {
            deps.workspace_registry->scan(projects_dir());
            workspaces = deps.workspace_registry->list();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        // Synchronous borrowed callback; never retained after this request.
        auto report = diagnose_session_data(projects_dir(), workspaces, [this, deadline] {
            return shutdown_requested.load() || std::chrono::steady_clock::now() >= deadline;
        });
        crow::response response(report.dump());
        response.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(response));
    });

    CROW_ROUTE(app, "/api/diagnostics/session-open").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        const auto invalid = [&] {
            crow::response response(400, R"({"error":"BAD_REQUEST","message":"Invalid session-open measurement"})");
            response.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(response));
        };
        if (req.body.size() > 4096) return invalid();
        const auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("session_id") || !body["session_id"].is_string()) return invalid();
        const auto sid = body["session_id"].get<std::string>();
        if (sid.empty() || sid.size() > 128 ||
            sid.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_") != std::string::npos) return invalid();
        for (const auto* key : {"elapsed_ms", "history_requests", "history_bytes"}) {
            if (!body.contains(key) || !body[key].is_number()) return invalid();
            const auto value = body[key].get<double>();
            if (!std::isfinite(value) || value < 0 || value > 1e12) return invalid();
        }
        // Explicit allowlist: never log extra fields submitted by a client.
        const nlohmann::json payload{
            {"operation", "session_open"}, {"session_id", sid},
            {"elapsed_ms", body["elapsed_ms"]}, {"history_requests", body["history_requests"]},
            {"history_bytes", body["history_bytes"]},
        };
        const auto level = body["elapsed_ms"].get<double>() > 500 ? LogLevel::Warn : LogLevel::Dbg;
        Logger::instance().log(level, __FILE__, __LINE__, "[session-load] " + payload.dump());
        return with_cors(req, crow::response(204));
    });
}

} // namespace acecode::web
