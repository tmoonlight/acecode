#include "web/server_impl.hpp"
#include "web/handlers/tool_preamble_handler.hpp"
#include "config/config_mutation.hpp"
#include "session_host/session_registry.hpp"

namespace acecode::web {
using nlohmann::json;

// 「具体进度提示」开关(设置 > 常规 > 工作模式,openspec add-tool-preamble)。
// 数据在 config.json 的 agent_loop.tool_preamble;PUT 是 patch 语义,落盘后
// 直接下发到每个活跃会话(AgentLoop 每次用时取快照,不必等回合边界)。
void WebServer::Impl::register_tool_preamble() {
    const auto respond = [this](const crow::request& req, int status, const json& body) {
        crow::response response(status);
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        response.body = body.dump();
        return with_cors(req, std::move(response));
    };

    CROW_ROUTE(app, "/api/config/tool-preamble").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/config/tool-preamble").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        std::shared_lock<std::shared_mutex> config_lock(app_config_mu);
        return respond(req, 200,
                       tool_preamble_snapshot(deps.app_config->agent_loop.tool_preamble));
    });

    CROW_ROUTE(app, "/api/config/tool-preamble").methods(crow::HTTPMethod::PUT)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        const auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) return respond(req, 400, {{"error", "BAD_JSON"}});

        std::lock_guard<std::shared_mutex> config_lock(app_config_mu);
        ToolPreambleConfig next;
        std::string error;
        if (!parse_tool_preamble_request(body, deps.app_config->agent_loop.tool_preamble,
                                         next, error)) {
            return respond(req, 400, {{"error", "BAD_REQUEST"}, {"message", error}});
        }
        const auto result = mutate_config(
            [&next](AppConfig& cfg, std::string&) {
                if (cfg.agent_loop.tool_preamble == next) return false;
                cfg.agent_loop.tool_preamble = next;
                return true;
            },
            deps.config_path, deps.app_config);
        if (!result.ok) {
            return respond(req, 500, {{"error", "CONFIG_FAILED"},
                                      {"message", "could not read or save tool preamble settings"}});
        }
        deps.app_config->agent_loop.tool_preamble = result.config.agent_loop.tool_preamble;
        if (deps.session_registry) {
            deps.session_registry->refresh_tool_preamble_config(
                deps.app_config->agent_loop.tool_preamble);
        }
        return respond(req, 200,
                       tool_preamble_snapshot(deps.app_config->agent_loop.tool_preamble));
    });
}

} // namespace acecode::web
