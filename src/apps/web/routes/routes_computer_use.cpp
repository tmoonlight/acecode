#include "web/server_impl.hpp"
#include "web/handlers/computer_use_handler.hpp"
#include "computer_use/runtime.hpp"
#include "computer_use/availability.hpp"
#include "config/config_mutation.hpp"
#include "config/saved_models_revision.hpp"
#include "tool/computer_use_tool.hpp"

namespace acecode::web {
using nlohmann::json;

void WebServer::Impl::refresh_computer_use_tool_locked() {
    if (!deps.app_config) return;
    // Publish appearance before enabling; style-only edits preserve the worker
    // and its current observation while the next request picks up the new style.
    computer_use::set_pointer_appearance(deps.app_config->computer_use.pointer_style,
                                        deps.app_config->computer_use.pointer_color);
    computer_use::set_enabled(deps.app_config->computer_use.enabled && computer_use_supported());
    if (deps.tools) refresh_computer_use_tools(*deps.tools, *deps.app_config);
}

void WebServer::Impl::register_computer_use() {
    const auto respond = [this](const crow::request& req, int status, const json& body) {
        crow::response response(status);
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        response.body = body.dump();
        return with_cors(req, std::move(response));
    };
    CROW_ROUTE(app, "/api/config/computer-use").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/config/computer-use").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        json settings;
        { std::shared_lock<std::shared_mutex> lock(app_config_mu); settings = computer_use_settings(*deps.app_config); }
        settings["availability"] = computer_use::availability();
        return respond(req, 200, settings);
    });

    CROW_ROUTE(app, "/api/config/computer-use").methods(crow::HTTPMethod::PUT)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        const auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) return respond(req, 400, {{"error", "BAD_JSON"}});
        if (!computer_use_supported() && body.is_object() && body.contains("enabled") &&
            body["enabled"].is_boolean() && body["enabled"].get<bool>()) {
            return respond(req, 400, {{"error", "COMPUTER_USE_PLATFORM_UNSUPPORTED"}});
        }
        std::unique_lock<std::shared_mutex> lock(app_config_mu);
        const bool was_enabled = deps.app_config->computer_use.enabled;
        const auto result = mutate_config([&](AppConfig& candidate, std::string& error) {
            return apply_computer_use_settings(candidate, body, error);
        }, deps.config_path, deps.app_config);
        if (!result.ok) {
            const bool invalid = result.error_kind == ConfigMutationErrorKind::Validation;
            return respond(req, invalid ? 400 : 500,
                {{"error", invalid ? "BAD_REQUEST" : "PERSIST_FAILED"},
                 {"message", invalid ? result.error : "Could not save computer use settings"}});
        }
        deps.app_config->computer_use = result.config.computer_use;
        if (was_enabled != deps.app_config->computer_use.enabled) refresh_computer_use_tool_locked();
        else computer_use::set_pointer_appearance(deps.app_config->computer_use.pointer_style,
                                                 deps.app_config->computer_use.pointer_color);
        publish_live_saved_models(*deps.app_config, result.config.saved_models);
        auto settings = computer_use_settings(*deps.app_config);
        lock.unlock();
        settings["availability"] = computer_use::availability();
        return respond(req, 200, settings);
    });

    CROW_ROUTE(app, "/api/config/computer-use/permissions").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/config/computer-use/permissions").methods(crow::HTTPMethod::POST)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        const auto body = json::parse(req.body, nullptr, false);
        if (!body.is_object() || body.size() != 1 || !body.contains("permission") || !body["permission"].is_string() ||
            (body["permission"] != "accessibility" && body["permission"] != "screen_recording"))
            return respond(req, 400, {{"error", "COMPUTER_USE_INVALID_PERMISSION"}});
        auto availability = computer_use::request_permission(body["permission"].get<std::string>());
        json settings;
        { std::shared_lock<std::shared_mutex> lock(app_config_mu); settings = computer_use_settings(*deps.app_config); }
        settings["availability"] = std::move(availability);
        return respond(req, 200, settings);
    });
}

} // namespace acecode::web
