#include "web/server_impl.hpp"
#include "web/handlers/tool_rewrites_handler.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>

namespace acecode::web {
using nlohmann::json;

// 「工具重写」设置(Settings > 工具 > 工具重写)。数据不在 config.json 里,
// 单独存 <data_dir>/tool-rewrites.json(data_dir = config_path 所在目录),
// GET 每次从磁盘重读,外部手改文件也能立刻在设置页看到;PUT 整体替换、
// 原子落盘后立即发布到进程,下一次模型请求就用新名字。
void WebServer::Impl::register_tool_rewrites() {
    const auto respond = [this](const crow::request& req, int status, const json& body) {
        crow::response response(status);
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        response.body = body.dump();
        return with_cors(req, std::move(response));
    };
    const auto settings_path = [this]() -> std::string {
        if (deps.config_path.empty()) return {};
        const auto data_dir = path_from_utf8(deps.config_path).parent_path();
        return tool_rewrites::settings_path(path_to_utf8(data_dir));
    };
    const auto registered_tools = [this]() {
        return deps.tools ? deps.tools->get_registered_tools()
                          : std::vector<RegisteredToolInfo>{};
    };

    CROW_ROUTE(app, "/api/config/tool-rewrites").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/config/tool-rewrites").methods(crow::HTTPMethod::GET)
    ([this, respond, settings_path, registered_tools](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        const std::string path = settings_path();
        if (path.empty()) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        std::lock_guard<std::mutex> lock(tool_rewrites_mu);
        std::string warning;
        const auto settings = tool_rewrites::load_settings(path, &warning);
        return respond(req, 200,
                       tool_rewrites_snapshot(settings, registered_tools(), path, warning));
    });

    CROW_ROUTE(app, "/api/config/tool-rewrites").methods(crow::HTTPMethod::PUT)
    ([this, respond, settings_path, registered_tools](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        const std::string path = settings_path();
        if (path.empty()) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        const auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) return respond(req, 400, {{"error", "BAD_JSON"}});

        std::lock_guard<std::mutex> lock(tool_rewrites_mu);
        const auto tools = registered_tools();
        tool_rewrites::ToolRewriteSettings settings;
        std::string error;
        if (!parse_tool_rewrites_request(body, tools, settings, error)) {
            return respond(req, 400, {{"error", "BAD_REQUEST"}, {"message", error}});
        }
        if (!tool_rewrites::save_settings(path, settings, &error)) {
            return respond(req, 500, {{"error", "PERSIST_FAILED"}, {"message", error}});
        }
        if (!tool_rewrites::apply_to_process(settings, &error)) {
            // 落盘已成功,发布失败只可能是校验不一致 —— 前面已经校验过,
            // 这里属于防御;报 500 让用户重试而不是假装生效。
            return respond(req, 500, {{"error", "APPLY_FAILED"}, {"message", error}});
        }
        return respond(req, 200, tool_rewrites_snapshot(settings, tools, path));
    });
}

} // namespace acecode::web
