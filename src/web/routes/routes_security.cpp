// routes_security.cpp — 安全中心(openspec add-security-center)
//
//   GET/PUT  /api/config/sandbox            沙盒总开关 / 网络 / 默认禁止名单 / 三张清单
//   GET/PUT  /api/security/exec-rules       全局规则目录;只有两个托管文件可写
//   GET      /api/security/audit            审计列表(筛选 + 游标分页)
//   GET      /api/security/audit/summary    计数 + 最近被拦路径
//   GET      /api/security/audit/export     JSONL / CSV 附件
//   DELETE   /api/security/audit            清空
//
// 沙盒配置写 config.json(app_config_mu 独占)后经 SessionRegistry 下发到活跃
// 会话;托管规则文件由 exec_rules_mu 串行化,写完让活跃会话重载规则。审计存储
// 是进程级单例,未配置(测试 fixture / 数据目录不可写)时相关端点返回 503。
#include "web/server_impl.hpp"
#include "web/handlers/security_handler.hpp"
#include "sandbox/exec_rules.hpp"
#include "sandbox/sandbox_backend.hpp"
#include "security/audit_log.hpp"
#include "session/session_registry.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <map>
#include <shared_mutex>

namespace acecode::web {
using nlohmann::json;

void WebServer::Impl::register_security() {
    const auto respond = [this](const crow::request& req, int status, const json& body) {
        crow::response response(status);
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        response.body = body.dump();
        return with_cors(req, std::move(response));
    };
    const auto bad_request = [respond](const crow::request& req, const std::string& message,
                                       const std::string& field = {}) {
        json body{{"error", "BAD_REQUEST"}, {"message", message}};
        if (!field.empty()) body["field"] = field;
        return respond(req, 400, body);
    };
    // 数据目录:与 tool-rewrites 同款,取 config_path 所在目录(测试 fixture 是临时目录),
    // 没有显式路径时退到进程数据目录。
    const auto data_dir = [this]() -> std::string {
        if (!deps.config_path.empty()) {
            return path_to_utf8(path_from_utf8(deps.config_path).parent_path());
        }
        return get_acecode_dir();
    };
    const auto rules_dir = [data_dir]() -> std::string {
        return path_to_utf8(path_from_utf8(data_dir()) / "rules");
    };
    const auto probe_for = [](const SandboxConfig& sandbox) {
        return sandbox::probe_backend(sandbox.windows_backend == "mxc"
            ? sandbox::WindowsBackendChoice::Mxc : sandbox::WindowsBackendChoice::RestrictedToken);
    };
    const auto query_params = [](const crow::request& req) {
        std::map<std::string, std::string> params;
        for (const char* key : {"category", "decision", "since_ms", "before_id", "q", "limit", "format"}) {
            if (const char* value = req.url_params.get(key)) params[key] = value;
        }
        return params;
    };

    for (const char* path : {"/api/config/sandbox", "/api/security/exec-rules", "/api/security/audit",
                             "/api/security/audit/summary", "/api/security/audit/export"}) {
        app.route_dynamic(path).methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) { return cors_preflight(req); });
    }

    // ---- 沙盒配置 ----

    CROW_ROUTE(app, "/api/config/sandbox").methods(crow::HTTPMethod::GET)
    ([this, respond, probe_for](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        SandboxConfig sandbox;
        {
            std::shared_lock<std::shared_mutex> lock(app_config_mu);
            sandbox = deps.app_config->sandbox;
        }
        return respond(req, 200, sandbox_settings_snapshot(sandbox, probe_for(sandbox)));
    });

    CROW_ROUTE(app, "/api/config/sandbox").methods(crow::HTTPMethod::PUT)
    ([this, respond, bad_request, probe_for](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        const auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) return respond(req, 400, {{"error", "BAD_JSON"}});

        SandboxConfig applied;
        {
            std::lock_guard<std::shared_mutex> lock(app_config_mu);
            SandboxConfig next = deps.app_config->sandbox;
            std::string error;
            std::string field;
            if (!parse_sandbox_settings_request(body, next, error, field)) {
                return bad_request(req, error, field);
            }
            const SandboxConfig previous = deps.app_config->sandbox;
            deps.app_config->sandbox = next;
            try {
                if (!deps.config_path.empty()) save_config(*deps.app_config, deps.config_path);
                else save_config(*deps.app_config);
            } catch (const std::exception& e) {
                deps.app_config->sandbox = previous;
                return respond(req, 500, {{"error", "PERSIST_FAILED"},
                                          {"message", std::string("persist failed: ") + e.what()}});
            }
            applied = deps.app_config->sandbox;
        }
        std::size_t refreshed = 0;
        if (deps.session_registry) refreshed = deps.session_registry->refresh_sandbox_config(applied);
        json snapshot = sandbox_settings_snapshot(applied, probe_for(applied));
        snapshot["refreshed_sessions"] = refreshed;
        return respond(req, 200, snapshot);
    });

    // ---- 命令规则 ----

    CROW_ROUTE(app, "/api/security/exec-rules").methods(crow::HTTPMethod::GET)
    ([this, respond, rules_dir](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        const std::string dir = rules_dir();
        std::lock_guard<std::mutex> lock(exec_rules_mu);
        return respond(req, 200, exec_rules_snapshot(dir, read_exec_rules_dir(dir)));
    });

    CROW_ROUTE(app, "/api/security/exec-rules").methods(crow::HTTPMethod::PUT)
    ([this, respond, bad_request, rules_dir](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        const auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) return respond(req, 400, {{"error", "BAD_JSON"}});
        std::map<std::string, std::vector<sandbox::PrefixRule>> files;
        std::string error;
        if (!parse_exec_rules_request(body, files, error)) return bad_request(req, error);
        const std::string dir = rules_dir();
        {
            std::lock_guard<std::mutex> lock(exec_rules_mu);
            for (const auto& [name, rules] : files) {
                const std::string file = path_to_utf8(path_from_utf8(dir) / name);
                const std::string write_error = sandbox::write_rules_file(file, rules);
                if (!write_error.empty()) {
                    return respond(req, 500, {{"error", "PERSIST_FAILED"}, {"message", write_error},
                                              {"file", name}});
                }
            }
        }
        std::size_t refreshed = 0;
        if (deps.session_registry) refreshed = deps.session_registry->refresh_exec_rules();
        std::lock_guard<std::mutex> lock(exec_rules_mu);
        json snapshot = exec_rules_snapshot(dir, read_exec_rules_dir(dir));
        snapshot["refreshed_sessions"] = refreshed;
        return respond(req, 200, snapshot);
    });

    // ---- 审计 ----

    const auto audit_unavailable = [respond](const crow::request& req) {
        return respond(req, 503, {{"error", "AUDIT_UNAVAILABLE"},
                                  {"message", "audit log is not configured in this process"}});
    };

    CROW_ROUTE(app, "/api/security/audit").methods(crow::HTTPMethod::GET)
    ([this, respond, bad_request, audit_unavailable, query_params](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        auto& log = security::audit_log();
        if (!log.available()) return audit_unavailable(req);
        security::AuditQuery query;
        std::string error;
        if (!parse_audit_query(query_params(req), query, error)) return bad_request(req, error);
        const auto page = log.query(query, &error);
        if (!error.empty()) return respond(req, 500, {{"error", "AUDIT_QUERY_FAILED"}, {"message", error}});
        return respond(req, 200, audit_page_to_json(page));
    });

    CROW_ROUTE(app, "/api/security/audit/summary").methods(crow::HTTPMethod::GET)
    ([this, respond, audit_unavailable](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        auto& log = security::audit_log();
        if (!log.available()) return audit_unavailable(req);
        std::string error;
        const auto summary = log.summary(20, &error);
        if (!error.empty()) return respond(req, 500, {{"error", "AUDIT_QUERY_FAILED"}, {"message", error}});
        json body = audit_summary_to_json(summary);
        body["path"] = log.path();
        body["max_entries"] = log.max_entries();
        return respond(req, 200, body);
    });

    CROW_ROUTE(app, "/api/security/audit/export").methods(crow::HTTPMethod::GET)
    ([this, respond, bad_request, audit_unavailable, query_params](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        auto& log = security::audit_log();
        if (!log.available()) return audit_unavailable(req);
        auto params = query_params(req);
        const std::string format = params.count("format") ? params["format"] : std::string("jsonl");
        if (format != "jsonl" && format != "csv") return bad_request(req, "format must be jsonl or csv", "format");
        params.erase("limit");
        security::AuditQuery query;
        std::string error;
        if (!parse_audit_query(params, query, error)) return bad_request(req, error);
        query.limit = security::AuditLog::kMaxQueryLimit;
        const auto page = log.query(query, &error);
        if (!error.empty()) return respond(req, 500, {{"error", "AUDIT_QUERY_FAILED"}, {"message", error}});
        crow::response response(200);
        response.add_header("Content-Type", format == "csv" ? "text/csv; charset=utf-8"
                                                            : "application/x-ndjson; charset=utf-8");
        response.add_header("Content-Disposition",
            "attachment; filename=\"" + audit_export_filename(format, security::audit_now_ms()) + "\"");
        response.add_header("Cache-Control", "no-store");
        response.body = format == "csv" ? security::render_audit_csv(page.entries)
                                        : security::render_audit_jsonl(page.entries);
        return with_cors(req, std::move(response));
    });

    CROW_ROUTE(app, "/api/security/audit").methods(crow::HTTPMethod::Delete)
    ([this, respond, audit_unavailable](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        auto& log = security::audit_log();
        if (!log.available()) return audit_unavailable(req);
        std::string error;
        if (!log.clear(&error)) return respond(req, 500, {{"error", "AUDIT_CLEAR_FAILED"}, {"message", error}});
        return respond(req, 200, {{"ok", true}, {"total", 0}});
    });
}

} // namespace acecode::web
