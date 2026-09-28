#include "web/server_impl.hpp"
#include "web/handlers/summary_generation_handler.hpp"
#include "config/config_mutation.hpp"
#include "config/saved_models_revision.hpp"

namespace acecode::web {
using nlohmann::json;

void WebServer::Impl::register_summary_generation() {
    const auto respond = [this](const crow::request& req, int status, const json& body) {
        crow::response response(status);
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        response.body = body.dump();
        return with_cors(req, std::move(response));
    };
    CROW_ROUTE(app, "/api/config/summary-generation").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/config/summary-generation").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        std::shared_lock<std::shared_mutex> lock(app_config_mu);
        return respond(req, 200, summary_generation_settings(*deps.app_config));
    });

    CROW_ROUTE(app, "/api/config/summary-generation").methods(crow::HTTPMethod::PUT)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        const auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) return respond(req, 400, {{"error", "BAD_JSON"}});
        std::lock_guard<std::shared_mutex> lock(app_config_mu);
        const auto result = mutate_config([&](AppConfig& candidate, std::string& error) {
            return apply_summary_generation_settings(candidate, body, error);
        }, deps.config_path, deps.app_config);
        if (!result.ok) {
            const bool invalid = result.error_kind == ConfigMutationErrorKind::Validation;
            return respond(req, invalid ? 400 : 500,
                {{"error", invalid ? "BAD_REQUEST" : "PERSIST_FAILED"}});
        }
        deps.app_config->summary_generation = result.config.summary_generation;
        publish_live_saved_models(*deps.app_config, result.config.saved_models);
        return respond(req, 200, summary_generation_settings(*deps.app_config));
    });
}

} // namespace acecode::web
