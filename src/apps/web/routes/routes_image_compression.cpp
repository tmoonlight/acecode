#include "web/server_impl.hpp"
#include "web/handlers/image_compression_handler.hpp"
#include "config/config_mutation.hpp"
#include "image/image_processor.hpp"

namespace acecode::web {
using nlohmann::json;

// 「压缩图片」开关(设置 > 常规)。数据在 config.json 的 image_compression;PUT 是
// patch 语义,落盘后立即改进程级开关,下一次模型请求就按新设置发送图片。
// 同机其它 daemon 在下一次发消息前经 refresh_model_config_if_changed 跟上。
void WebServer::Impl::register_image_compression() {
    const auto respond = [this](const crow::request& req, int status, const json& body) {
        crow::response response(status);
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        response.body = body.dump();
        return with_cors(req, std::move(response));
    };

    CROW_ROUTE(app, "/api/config/image-compression").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/config/image-compression").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        std::shared_lock<std::shared_mutex> config_lock(app_config_mu);
        return respond(req, 200, image_compression_snapshot(deps.app_config->image_compression));
    });

    CROW_ROUTE(app, "/api/config/image-compression").methods(crow::HTTPMethod::PUT)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        const auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) return respond(req, 400, {{"error", "BAD_JSON"}});

        std::lock_guard<std::shared_mutex> config_lock(app_config_mu);
        ImageCompressionConfig next;
        std::string error;
        if (!parse_image_compression_request(body, deps.app_config->image_compression,
                                             next, error)) {
            return respond(req, 400, {{"error", "BAD_REQUEST"}, {"message", error}});
        }
        const auto result = mutate_config(
            [&next](AppConfig& cfg, std::string&) {
                if (cfg.image_compression == next) return false;
                cfg.image_compression = next;
                return true;
            },
            deps.config_path, deps.app_config);
        if (!result.ok) {
            return respond(req, 500, {{"error", "CONFIG_FAILED"},
                                      {"message", "could not read or save image compression settings"}});
        }
        deps.app_config->image_compression = result.config.image_compression;
        image::set_model_image_compression_enabled(deps.app_config->image_compression.enabled);
        return respond(req, 200, image_compression_snapshot(deps.app_config->image_compression));
    });
}

} // namespace acecode::web
