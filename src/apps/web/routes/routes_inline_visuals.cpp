#include "web/server_impl.hpp"
#include "web/handlers/inline_visuals_handler.hpp"
#include "config/config_mutation.hpp"

namespace acecode::web {
using nlohmann::json;

// 「可视化图表」开关(设置 > 常规)。数据在 config.json 的 inline_visuals;PUT 是
// patch 语义,落盘后本进程下一次请求组装系统提示时就读到新值(提示配置快照每次
// 请求都读 AppConfig)。同机其它 daemon 在下一次发消息前经
// refresh_model_config_if_changed 跟上。
void WebServer::Impl::register_inline_visuals() {
    const auto respond = [this](const crow::request& req, int status, const json& body) {
        crow::response response(status);
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        response.body = body.dump();
        return with_cors(req, std::move(response));
    };

    CROW_ROUTE(app, "/api/config/inline-visuals").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/config/inline-visuals").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        std::shared_lock<std::shared_mutex> config_lock(app_config_mu);
        return respond(req, 200, inline_visuals_snapshot(deps.app_config->inline_visuals));
    });

    CROW_ROUTE(app, "/api/config/inline-visuals").methods(crow::HTTPMethod::PUT)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        const auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) return respond(req, 400, {{"error", "BAD_JSON"}});

        std::lock_guard<std::shared_mutex> config_lock(app_config_mu);
        InlineVisualsConfig next;
        std::string error;
        if (!parse_inline_visuals_request(body, deps.app_config->inline_visuals, next, error)) {
            return respond(req, 400, {{"error", "BAD_REQUEST"}, {"message", error}});
        }
        const auto result = mutate_config(
            [&next](AppConfig& cfg, std::string&) {
                if (cfg.inline_visuals == next) return false;
                cfg.inline_visuals = next;
                return true;
            },
            deps.config_path, deps.app_config);
        if (!result.ok) {
            return respond(req, 500, {{"error", "CONFIG_FAILED"},
                                      {"message", "could not read or save inline visuals settings"}});
        }
        deps.app_config->inline_visuals = result.config.inline_visuals;
        return respond(req, 200, inline_visuals_snapshot(deps.app_config->inline_visuals));
    });
}

} // namespace acecode::web
