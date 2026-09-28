#include "web/server_impl.hpp"

namespace acecode::web {
namespace {

crow::response suggestion_response(const TaskSuggestionServiceResult& result) {
    const auto body = result.ok ? result.value
                               : nlohmann::json{{"error", result.error}};
    crow::response response(result.http_status, body.dump());
    response.add_header("Content-Type", "application/json");
    return response;
}

} // namespace

void WebServer::Impl::register_task_suggestions() {
    CROW_ROUTE(app, "/api/sessions/<string>/suggestions")
        .methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&) {
        return cors_preflight(req);
    });
    CROW_ROUTE(app, "/api/sessions/<string>/suggestions/<string>/accept")
        .methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&, const std::string&) {
        return cors_preflight(req);
    });
    CROW_ROUTE(app, "/api/sessions/<string>/suggestions/<string>/dismiss")
        .methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&, const std::string&) {
        return cors_preflight(req);
    });
    CROW_ROUTE(app, "/api/sessions/<string>/suggestions")
        .methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req, const std::string& source_id) {
        if (auto rejection = require_auth(req)) return std::move(*rejection);
        if (!deps.task_suggestions) return with_cors(req, crow::response(404));
        return with_cors(req, suggestion_response(deps.task_suggestions->list(source_id)));
    });
    CROW_ROUTE(app, "/api/sessions/<string>/suggestions/<string>/accept")
        .methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req, const std::string& source_id,
           const std::string& suggestion_id) {
        if (auto rejection = require_auth(req)) return std::move(*rejection);
        if (auto rejection = reject_if_migrating(req)) return std::move(*rejection);
        if (!deps.task_suggestions) return with_cors(req, crow::response(404));
        const auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("location") ||
            !body["location"].is_string() || body.size() != 1) {
            return with_cors(req, suggestion_response(
                {false, 400, {}, "location is required (worktree or current_branch)"}));
        }
        return with_cors(req, suggestion_response(deps.task_suggestions->accept(
            source_id, suggestion_id, body["location"].get<std::string>())));
    });
    CROW_ROUTE(app, "/api/sessions/<string>/suggestions/<string>/dismiss")
        .methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req, const std::string& source_id,
           const std::string& suggestion_id) {
        if (auto rejection = require_auth(req)) return std::move(*rejection);
        if (auto rejection = reject_if_migrating(req)) return std::move(*rejection);
        if (!deps.task_suggestions) return with_cors(req, crow::response(404));
        return with_cors(req, suggestion_response(
            deps.task_suggestions->dismiss(source_id, suggestion_id)));
    });
}

} // namespace acecode::web
