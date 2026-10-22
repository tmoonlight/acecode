#include "web/server_impl.hpp"
#include "web/auth.hpp"
#include "web/handlers/agent_browser_top_sites.hpp"

#include "environment/data_dir_migration.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>

namespace acecode::web {
using nlohmann::json;
namespace top_sites = agent_browser_top_sites;

namespace {

// 进程内串行化整份「读-改-写」。Desktop 只连一个共享 daemon,Agent Browser
// 的访问只从它的页面上报,不需要跨进程锁;atomic_write_file 保证文件本身不写坏。
std::mutex& top_sites_mutex() {
    static std::mutex mu;
    return mu;
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::size_t parse_limit(const char* raw) {
    if (!raw || !*raw) return top_sites::kDefaultDisplayLimit;
    try {
        const long value = std::stol(raw);
        if (value <= 0) return top_sites::kDefaultDisplayLimit;
        return std::min<std::size_t>(static_cast<std::size_t>(value),
                                     top_sites::kMaxDisplayLimit);
    } catch (...) {
        return top_sites::kDefaultDisplayLimit;
    }
}

} // namespace

// Agent Browser 新标签页的「推荐」(常访问网站)。浏览记录属于隐私数据,
// 只给本机直连客户端(远程 Web 代理转发进来的 127.0.0.2 也拒绝);Agent Browser
// 本来就只存在于本机 Desktop。
void WebServer::Impl::register_agent_browser() {
    const auto respond = [this](const crow::request& req, int status, const json& body) {
        crow::response response(status);
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        response.body = body.dump(-1, ' ', false, json::error_handler_t::replace);
        return with_cors(req, std::move(response));
    };
    const auto gate = [this, respond](const crow::request& req) -> std::optional<crow::response> {
        if (!is_trusted_local_client_address(req.remote_ip_address)) {
            log_unauthorized(req.url, req.remote_ip_address, "agent browser non-local");
            return respond(req, 403, {{"error", "LOCAL_ONLY"}});
        }
        if (auto rejected = require_auth(req)) return rejected;
        if (deps.config_path.empty()) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        return std::nullopt;
    };
    const auto store_path = [this]() {
        const auto data_dir = path_from_utf8(deps.config_path).parent_path();
        return top_sites::file_path(path_to_utf8(data_dir));
    };
    // 修改并落盘;mutate 返回 false 表示无变化。数据目录迁移期间拒绝写入,
    // 否则迁移的变更复检会因为这个文件被改而失败。
    const auto mutate = [this, respond, store_path](
                            const crow::request& req,
                            const std::function<bool(json&)>& change) {
        std::shared_lock<std::shared_mutex> migration_lock(
            environment::data_dir_write_mutex());
        if (environment::data_dir_writes_blocked()) {
            return respond(req, 409, {{"error", "DATA_DIR_MIGRATION_ACTIVE"}});
        }
        std::lock_guard<std::mutex> lock(top_sites_mutex());
        const auto path = store_path();
        std::string warning;
        auto doc = top_sites::load(path, &warning);
        if (!warning.empty()) LOG_WARN("[agent-browser] top sites reset: " + warning);
        if (!change(doc)) return respond(req, 200, {{"ok", true}, {"changed", false}});
        std::string error;
        if (!top_sites::save(path, doc, &error)) {
            LOG_WARN("[agent-browser] failed to save top sites: " + error);
            return respond(req, 500, {{"error", "PERSIST_FAILED"}});
        }
        return respond(req, 200, {{"ok", true}, {"changed", true}});
    };

    for (const char* path : {"/api/agent-browser/top-sites",
                             "/api/agent-browser/visits",
                             "/api/agent-browser/top-sites/dismiss",
                             "/api/agent-browser/top-sites/restore"}) {
        app.route_dynamic(path).methods(crow::HTTPMethod::Options)(
            [this](const crow::request& req) { return cors_preflight(req); });
    }

    CROW_ROUTE(app, "/api/agent-browser/top-sites").methods(crow::HTTPMethod::GET)
    ([gate, respond, store_path](const crow::request& req) {
        if (auto rejected = gate(req)) return std::move(*rejected);
        json doc;
        {
            std::lock_guard<std::mutex> lock(top_sites_mutex());
            doc = top_sites::load(store_path());
        }
        const auto limit = parse_limit(req.url_params.get("limit"));
        return respond(req, 200, {{"sites", top_sites::display_sites(doc, limit, now_ms())}});
    });

    // body {url, title?, favicon?, new_visit?}:新导航 new_visit=true 计一次访问,
    // 同一次访问后到的标题 / 图标用 new_visit=false 补充。
    CROW_ROUTE(app, "/api/agent-browser/visits").methods(crow::HTTPMethod::POST)
    ([gate, respond, mutate](const crow::request& req) {
        if (auto rejected = gate(req)) return std::move(*rejected);
        const auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            return respond(req, 400, {{"error", "BAD_JSON"}});
        }
        top_sites::Visit visit;
        visit.url = body.value("url", std::string());
        visit.title = body.value("title", std::string());
        visit.favicon = body.value("favicon", std::string());
        visit.new_visit = body.value("new_visit", true);
        if (top_sites::normalize_url(visit.url).empty()) {
            return respond(req, 400, {{"error", "BAD_URL"}});
        }
        return mutate(req, [&visit](json& doc) {
            return top_sites::record_visit(doc, visit, now_ms());
        });
    });

    const auto url_change = [gate, respond, mutate](
                                const crow::request& req,
                                bool (*change)(json&, const std::string&)) {
        if (auto rejected = gate(req)) return std::move(*rejected);
        const auto body = json::parse(req.body, nullptr, false);
        const auto url = body.is_object() ? body.value("url", std::string()) : std::string();
        if (top_sites::normalize_url(url).empty()) {
            return respond(req, 400, {{"error", "BAD_URL"}});
        }
        return mutate(req, [&url, change](json& doc) { return change(doc, url); });
    };

    CROW_ROUTE(app, "/api/agent-browser/top-sites/dismiss").methods(crow::HTTPMethod::POST)
    ([url_change](const crow::request& req) {
        return url_change(req, &top_sites::dismiss);
    });

    CROW_ROUTE(app, "/api/agent-browser/top-sites/restore").methods(crow::HTTPMethod::POST)
    ([url_change](const crow::request& req) {
        return url_change(req, &top_sites::restore);
    });
}

} // namespace acecode::web
