#include "web/server_impl.hpp"
#include "session_host/session_registry.hpp"
#include "tool/background_tasks.hpp"

#include <algorithm>
#include <cstdlib>

namespace acecode::web {
using nlohmann::json;

// 会话的后台任务(后台命令 / 后台子代理):列表、输出末尾、停止,以及把正在
// 跑的前台工作转到后台。会话不在内存里时没有后台任务,列表返回空。
void WebServer::Impl::register_background_tasks() {
    const auto respond = [this](const crow::request& req, int status, const json& body) {
        crow::response response(status);
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        response.body = body.dump();
        return with_cors(req, std::move(response));
    };

    CROW_ROUTE(app, "/api/sessions/<string>/background-tasks").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/sessions/<string>/background-tasks").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.session_registry) return respond(req, 503, {{"error", "session registry unavailable"}});
        json tasks = json::array();
        if (auto entry = deps.session_registry->acquire(id); entry && entry->loop) {
            for (const auto& info : entry->loop->background_tasks().list()) {
                tasks.push_back(background_task_to_json(info));
            }
        }
        return respond(req, 200, {{"session_id", id}, {"tasks", tasks}});
    });

    CROW_ROUTE(app, "/api/sessions/<string>/background-tasks/<string>/output").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&, const std::string&) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/sessions/<string>/background-tasks/<string>/output").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req, const std::string& id, const std::string& task_id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.session_registry) return respond(req, 503, {{"error", "session registry unavailable"}});
        auto entry = deps.session_registry->acquire(id);
        if (!entry || !entry->loop) return respond(req, 404, {{"error", "unknown session"}});
        const auto info = entry->loop->background_tasks().find(task_id);
        if (!info) return respond(req, 404, {{"error", "unknown background task"}});
        std::size_t tail = 16 * 1024;
        if (const char* raw = req.url_params.get("tail")) {
            const long requested = std::strtol(raw, nullptr, 10);
            if (requested > 0) tail = static_cast<std::size_t>(std::min<long>(requested, 64 * 1024));
        }
        return respond(req, 200, {
            {"task", background_task_to_json(*info)},
            {"output", entry->loop->background_tasks().read_output_tail(task_id, tail)},
        });
    });

    CROW_ROUTE(app, "/api/sessions/<string>/background-tasks/<string>/stop").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&, const std::string&) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/sessions/<string>/background-tasks/<string>/stop").methods(crow::HTTPMethod::POST)
    ([this, respond](const crow::request& req, const std::string& id, const std::string& task_id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.session_registry) return respond(req, 503, {{"error", "session registry unavailable"}});
        auto entry = deps.session_registry->acquire(id);
        if (!entry || !entry->loop) return respond(req, 404, {{"error", "unknown session"}});
        std::string error;
        if (!entry->loop->background_tasks().stop(task_id, error)) {
            return respond(req, 409, {{"error", error}});
        }
        return respond(req, 200, {{"ok", true}, {"task_id", task_id}});
    });

    CROW_ROUTE(app, "/api/sessions/<string>/background").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/sessions/<string>/background").methods(crow::HTTPMethod::POST)
    ([this, respond](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.session_registry) return respond(req, 503, {{"error", "session registry unavailable"}});
        auto entry = deps.session_registry->acquire(id);
        if (!entry || !entry->loop) return respond(req, 404, {{"error", "unknown session"}});
        const bool busy = entry->loop->request_background(BackgroundOrigin::UserRequest);
        return respond(req, 200, {{"ok", true}, {"busy", busy}});
    });
}

} // namespace acecode::web
