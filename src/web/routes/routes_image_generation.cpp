#include "../server_impl.hpp"
#include "../handlers/image_generation_handler.hpp"
#include "../../config/config_mutation.hpp"
#include "../../config/saved_models_revision.hpp"
#include "../../tool/image_generate/image_generate_tool.hpp"
#include "../../tool/image_generate/image_generation_client.hpp"
#include "../../tool/image_generate/image_generation_policy.hpp"

namespace acecode::web {
using nlohmann::json;

void WebServer::Impl::refresh_image_generation_tool_locked() {
    if (!deps.tools || !deps.app_config) return;
    // ToolExecutor swaps implementations under its own lock. An in-flight
    // invocation keeps its snapshot; subsequent calls use the new settings.
    if (auto tool = create_image_generate_tool(*deps.app_config)) {
        deps.tools->register_tool(*tool);
    } else {
        deps.tools->unregister_tool("image_generate", std::string{});
    }
}

void WebServer::Impl::register_image_generation() {
    const auto respond = [this](const crow::request& req, int status, const json& body) {
        crow::response response(status);
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        response.body = body.dump();
        return with_cors(req, std::move(response));
    };
    CROW_ROUTE(app, "/api/config/image-generation").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/config/image-generation/test").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/config/image-generation").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        std::shared_lock<std::shared_mutex> lock(app_config_mu);
        return respond(req, 200, image_generation_settings(*deps.app_config));
    });

    CROW_ROUTE(app, "/api/config/image-generation").methods(crow::HTTPMethod::PUT)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        const auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) return respond(req, 400, {{"error", "BAD_JSON"}});
        std::lock_guard<std::shared_mutex> lock(app_config_mu);
        const auto result = mutate_config([&](AppConfig& candidate, std::string& error) {
            return apply_image_generation_settings(candidate, body, error);
        }, deps.config_path, deps.app_config);
        if (!result.ok) {
            const bool invalid = result.error_kind == ConfigMutationErrorKind::Validation;
            return respond(req, invalid ? 400 : 500,
                {{"error", invalid ? "BAD_REQUEST" : "PERSIST_FAILED"},
                 {"message", invalid ? result.error : "Could not save image generation settings"}});
        }
        // Preserve runtime-only AppConfig fields, such as the listener port.
        deps.app_config->image_generation = result.config.image_generation;
        publish_live_saved_models(*deps.app_config, result.config.saved_models);
        refresh_image_generation_tool_locked();
        return respond(req, 200, image_generation_settings(*deps.app_config));
    });

    CROW_ROUTE(app, "/api/config/image-generation/test").methods(crow::HTTPMethod::POST)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        const auto body = json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("confirm_cost") ||
            !body["confirm_cost"].is_boolean() || !body["confirm_cost"].get<bool>()) {
            return respond(req, 400, {{"error", "IMAGE_COST_CONFIRMATION_REQUIRED"}});
        }
        std::unique_lock<std::mutex> testing(image_generation_test_mu, std::try_to_lock);
        if (!testing.owns_lock()) return respond(req, 409, {{"error", "IMAGE_TEST_BUSY"}});
        AppConfig candidate;
        {
            std::shared_lock<std::shared_mutex> lock(app_config_mu);
            candidate = *deps.app_config;
        }
        std::string error;
        if (!apply_image_generation_settings(candidate, body.value("config", json::object()), error)) {
            return respond(req, 400, {{"error", "BAD_REQUEST"}, {"message", error}});
        }
        candidate.image_generation.enabled = true;
        const auto endpoint = image_generation::resolve_endpoint(candidate);
        if (!endpoint.ok) return respond(req, 400, {{"error", "IMAGE_NOT_CONFIGURED"}});
        image_generation::ImageRequest request;
        request.base_url = endpoint.base_url;
        request.api_key = endpoint.api_key;
        request.model = image_generation::model_for_quality(
            candidate.image_generation, image_generation::Quality::Standard);
        request.prompt = "A small blue square centered on a plain white background.";
        request.timeout_ms = candidate.image_generation.timeout_ms;
        try {
            const auto result = image_generation::execute_image_request(request, &shutdown_requested);
            if (!result.ok) {
                // Upstreams may echo credentials or request bodies in errors.
                // Return stable categories instead of relaying that content.
                return respond(req, 502, {{"error", result.quota_error ? "IMAGE_QUOTA_ERROR" : "IMAGE_TEST_FAILED"}});
            }
            return respond(req, 200, {
                {"image_data_url", "data:" + result.mime_type + ";base64," + result.b64_data},
                {"width", result.width}, {"height", result.height},
                {"quality", "standard"}, {"model", request.model}});
        } catch (const std::exception&) {
            return respond(req, 502, {{"error", "IMAGE_TEST_FAILED"}});
        }
    });
}

} // namespace acecode::web
