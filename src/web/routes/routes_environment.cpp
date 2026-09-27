// routes_environment.cpp — Settings → 配置 页的环境端点
// (openspec: redesign-settings-config-section)。
//
//   GET/PUT  /api/config/toolchains            工具链目录
//   POST     /api/config/toolchains/detect     重新检测工具链
//   GET      /api/console/config               终端配置 + 解析结果(PUT 在 routes_pty.cpp)
//   POST     /api/console/config/detect        重新解析终端并落盘
//   GET      /api/config/data-dir              数据目录状态(含清理提示)
//   POST     /api/config/data-dir/migrate      启动迁移(202 / 400 / 409)
//   GET      /api/config/data-dir/migration    迁移进度
//   POST     /api/config/data-dir/cleanup      删除 / 保留旧目录
//   POST     /api/dialog/pick-folder|pick-file 原生选择对话框(无副作用)
#include "web/server_impl.hpp"

#include "environment/bootstrap.hpp"
#include "environment/data_dir_migration.hpp"
#include "environment/shell_command_line.hpp"
#include "environment/terminal_runtime.hpp"
#include "environment/toolchains.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>

namespace acecode::web {

using nlohmann::json;
namespace env = acecode::environment;

namespace {

crow::response json_response(int status, const json& body) {
    crow::response r(status);
    r.add_header("Content-Type", "application/json");
    // 出口兜底:错误文本里混进非法 UTF-8(GBK 的 OS 错误文本、BAD_JSON 里 nlohmann
    // parse_error 原样带出的请求体字节)时,默认 dump 抛 type_error.316,整个请求变 500;
    // 迁移失败的 progress 会一直留着那条错误,/migration 与 /data-dir 于是每次都 500,
    // 前端永远停在「迁移中」。replace 把非法字节换成 U+FFFD,响应永远是合法 JSON。
    r.body = body.dump(-1, ' ', false, json::error_handler_t::replace);
    return r;
}

crow::response error_response(int status, const std::string& code, const std::string& message) {
    return json_response(status, json{{"error", code}, {"message", message}});
}

std::string trim_copy(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                          s.front() == '\r' || s.front() == '\n')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n')) s.pop_back();
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
        return trim_copy(s);
    }
    return s;
}

json migration_progress_json(const env::MigrationProgress& p) {
    return json{
        {"state", p.state},
        {"target", p.target},
        {"copied_bytes", p.copied_bytes},
        {"total_bytes", p.total_bytes},
        {"skipped_files", p.skipped_files},
        {"error", p.error},
        {"restart_required", p.restart_required},
        {"started_at_ms", p.started_at_ms},
        {"finished_at_ms", p.finished_at_ms},
    };
}

json data_dir_status_json(const env::DataDirStatus& s) {
    json out{
        {"effective_dir", s.effective_dir},
        {"default_dir", s.default_dir},
        {"redirect_active", s.redirect_active},
        {"redirect_target", s.redirect_target},
        {"migrated_at_ms", s.migrated_at_ms},
        {"cleanup_pending", s.cleanup_pending},
    };
    if (s.previous_exists) {
        out["previous_dir"] = s.previous_dir;
        out["previous_size_bytes"] = s.previous_size_bytes;
    }
    if (s.cleanup_prompt) {
        out["cleanup"] = json{{"previous_dir", s.previous_dir},
                              {"size_bytes", s.previous_size_bytes}};
    }
    return out;
}

}  // namespace

json WebServer::Impl::toolchains_payload_locked() {
    json arr = json::array();
    const auto applied = env::applied_toolchain_dirs();
    for (const auto& id : env::toolchain_ids()) {
        const std::string dir = deps.app_config
            ? env::toolchain_dir(deps.app_config->toolchains, id) : std::string{};
        json item{{"id", id}, {"label", env::toolchain_label(id)}, {"dir", dir}};
        bool exists = false;
        std::string anchor;
        if (!dir.empty()) {
            std::error_code ec;
            exists = std::filesystem::is_directory(path_from_utf8(dir), ec) && !ec;
            if (exists) anchor = env::find_toolchain_anchor_in_dir(id, dir);
        }
        bool is_applied = false;
        for (const auto& [label, applied_dir] : applied) {
            if (!dir.empty() && applied_dir == dir) { is_applied = true; break; }
        }
        item["exists"] = exists;
        item["anchor"] = anchor;
        item["applied"] = is_applied;
        arr.push_back(std::move(item));
    }
    return json{{"toolchains", arr}};
}

json WebServer::Impl::console_config_payload_locked() {
    json out{{"default_shell", json()}, {"shell_paths", json::object()}};
    if (deps.app_config) {
        out["default_shell"] = deps.app_config->console.default_shell;
        for (const auto& [id, path] : deps.app_config->console.shell_paths) {
            out["shell_paths"][id] = path;
        }
    } else {
        out["default_shell"] = "";
    }
    json resolved = json::object();
    json candidates = json::array();
    if (const auto last = env::terminal().last()) {
        resolved = json{
            {"id", last->resolved.id},
            {"family", env::terminal_family_name(last->resolved.family)},
            {"program", last->resolved.program},
            {"console_command", last->resolved.console_command},
            {"usable", last->resolved.usable},
            {"fallback_reason", last->resolved.fallback_reason},
        };
        for (const auto& c : last->candidates) {
            candidates.push_back(json{
                {"id", c.id},
                {"label", c.label},
                {"family", env::terminal_family_name(c.family)},
                {"available", c.available},
                {"needs_path", c.needs_path},
                {"probed", c.probed},
                {"usable", c.usable},
                {"program", c.program},
                {"detected_path", c.detected_path},
                {"configured_path", c.configured_path},
                {"probe_error", c.probe_error},
            });
        }
    }
    out["resolved"] = resolved;
    out["candidates"] = candidates;
    return out;
}

std::optional<crow::response> WebServer::Impl::reject_if_migrating(const crow::request& req) {
    if (!env::data_dir_writes_blocked()) return std::nullopt;
    return with_cors(req, error_response(
        409, "DATA_DIR_MIGRATION_ACTIVE",
        "a data directory migration is in progress; wait for it to finish and restart ACECode"));
}

void WebServer::Impl::register_environment() {
    for (const char* path : {"/api/config/toolchains", "/api/config/toolchains/detect",
                             "/api/console/config/detect", "/api/config/data-dir",
                             "/api/config/data-dir/migrate", "/api/config/data-dir/migration",
                             "/api/config/data-dir/cleanup", "/api/dialog/pick-folder",
                             "/api/dialog/pick-file"}) {
        app.route_dynamic(path).methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) { return cors_preflight(req); });
    }

    // ---- toolchains ----------------------------------------------------

    CROW_ROUTE(app, "/api/config/toolchains").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req) {
        if (auto rej = require_auth(req)) return std::move(*rej);
        if (!deps.app_config) return with_cors(req, crow::response(503));
        std::shared_lock<std::shared_mutex> lock(app_config_mu);
        return with_cors(req, json_response(200, toolchains_payload_locked()));
    });

    CROW_ROUTE(app, "/api/config/toolchains").methods(crow::HTTPMethod::PUT)
    ([this](const crow::request& req) {
        if (auto rej = require_auth(req)) return std::move(*rej);
        if (!deps.app_config) return with_cors(req, crow::response(503));
        json body;
        try { body = json::parse(req.body); }
        catch (const std::exception& e) {
            return with_cors(req, error_response(400, "BAD_JSON", std::string("bad json: ") + e.what()));
        }
        if (!body.is_object()) {
            return with_cors(req, error_response(400, "BAD_JSON", "body must be an object"));
        }
        // 先在锁外校验(目录存在性),再进独占锁改配置。
        std::map<std::string, std::string> updates;
        for (const auto& id : env::toolchain_ids()) {
            if (!body.contains(id)) continue;
            if (!body[id].is_string()) {
                return with_cors(req, error_response(400, "INVALID_FIELD", id + " must be a string"));
            }
            std::string dir = trim_copy(body[id].get<std::string>());
            if (!dir.empty()) {
                std::error_code ec;
                const auto p = path_from_utf8(dir);
                if (!p.is_absolute()) {
                    return with_cors(req, json_response(400, json{
                        {"error", "DIRECTORY_NOT_ABSOLUTE"}, {"field", id},
                        {"message", "directory must be an absolute path"}}));
                }
                if (!std::filesystem::is_directory(p, ec) || ec) {
                    return with_cors(req, json_response(400, json{
                        {"error", "DIRECTORY_NOT_FOUND"}, {"field", id},
                        {"message", "directory does not exist: " + dir}}));
                }
            }
            updates[id] = dir;
        }

        ToolchainsConfig applied;
        {
            std::lock_guard<std::shared_mutex> lock(app_config_mu);
            const ToolchainsConfig previous = deps.app_config->toolchains;
            for (const auto& [id, dir] : updates) {
                env::toolchain_dir_ref(deps.app_config->toolchains, id) = dir;
            }
            try {
                if (!deps.config_path.empty()) save_config(*deps.app_config, deps.config_path);
                else save_config(*deps.app_config);
            } catch (const std::exception& e) {
                deps.app_config->toolchains = previous;
                return with_cors(req, error_response(
                    500, "PERSIST_FAILED", std::string("persist failed: ") + e.what()));
            }
            applied = deps.app_config->toolchains;
        }
        env::apply_toolchain_path(applied);
        std::shared_lock<std::shared_mutex> lock(app_config_mu);
        return with_cors(req, json_response(200, toolchains_payload_locked()));
    });

    CROW_ROUTE(app, "/api/config/toolchains/detect").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        if (auto rej = require_auth(req)) return std::move(*rej);
        if (!deps.app_config) return with_cors(req, crow::response(503));
        const auto detected = env::detect_toolchains();
        ToolchainsConfig applied;
        {
            std::lock_guard<std::shared_mutex> lock(app_config_mu);
            const ToolchainsConfig previous = deps.app_config->toolchains;
            if (env::merge_detected_toolchains(deps.app_config->toolchains, detected)) {
                try {
                    if (!deps.config_path.empty()) save_config(*deps.app_config, deps.config_path);
                    else save_config(*deps.app_config);
                } catch (const std::exception& e) {
                    deps.app_config->toolchains = previous;
                    return with_cors(req, error_response(
                        500, "PERSIST_FAILED", std::string("persist failed: ") + e.what()));
                }
            }
            applied = deps.app_config->toolchains;
        }
        env::apply_toolchain_path(applied);
        std::shared_lock<std::shared_mutex> lock(app_config_mu);
        json out = toolchains_payload_locked();
        json found = json::object();
        for (const auto& id : env::toolchain_ids()) {
            found[id] = !detected.dir_for(id).empty();
        }
        out["detected"] = found;
        return with_cors(req, json_response(200, out));
    });

    // ---- console / terminal ---------------------------------------------

    CROW_ROUTE(app, "/api/console/config").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req) {
        if (auto rej = require_auth(req)) return std::move(*rej);
        if (!deps.app_config) return with_cors(req, crow::response(503));
        std::shared_lock<std::shared_mutex> lock(app_config_mu);
        return with_cors(req, json_response(200, console_config_payload_locked()));
    });

    CROW_ROUTE(app, "/api/console/config/detect").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        if (auto rej = require_auth(req)) return std::move(*rej);
        if (!deps.app_config) return with_cors(req, crow::response(503));
        std::lock_guard<std::shared_mutex> lock(app_config_mu);
        AppConfig next = *deps.app_config;
        const auto resolution = env::resolve_terminal(next.console);
        if (resolution.resolved.usable) {
            if (env::persist_resolved_terminal(next.console, resolution.resolved)) {
                try {
                    if (!deps.config_path.empty()) save_config(next, deps.config_path);
                    else save_config(next);
                } catch (const std::exception& e) {
                    return with_cors(req, error_response(
                        500, "PERSIST_FAILED", std::string("persist failed: ") + e.what()));
                }
            }
            deps.app_config->console = next.console;
            if (deps.pty_registry) {
                deps.pty_registry->set_default_shell(resolution.resolved.console_command);
            }
        }
        env::terminal().publish(resolution);
        return with_cors(req, json_response(200, console_config_payload_locked()));
    });

    // ---- data directory ---------------------------------------------------

    auto migration_json_or_null = [this]() -> json {
        if (!data_dir_migration) return nullptr;
        const auto p = data_dir_migration->progress();
        if (!p) return nullptr;
        return migration_progress_json(*p);
    };

    CROW_ROUTE(app, "/api/config/data-dir").methods(crow::HTTPMethod::GET)
    ([this, migration_json_or_null](const crow::request& req) {
        if (auto rej = require_auth(req)) return std::move(*rej);
        json out = data_dir_status_json(env::data_dir_status(get_run_mode()));
        out["migration"] = migration_json_or_null();
        return with_cors(req, json_response(200, out));
    });

    CROW_ROUTE(app, "/api/config/data-dir/migrate").methods(crow::HTTPMethod::POST)
    ([this, migration_json_or_null](const crow::request& req) {
        if (auto rej = require_auth(req)) return std::move(*rej);
        if (!data_dir_migration) return with_cors(req, crow::response(503));
        json body;
        try { body = json::parse(req.body); }
        catch (const std::exception& e) {
            // 不把 e.what() 写进日志:nlohmann 会把请求体原始字节(可能是非法 UTF-8)带进来。
            LOG_WARN("[data-dir] migration refused: BAD_JSON");
            return with_cors(req, error_response(400, "BAD_JSON", std::string("bad json: ") + e.what()));
        }
        if (!body.is_object() || !body.contains("target") || !body["target"].is_string()) {
            LOG_WARN("[data-dir] migration refused: TARGET_REQUIRED (not a string)");
            return with_cors(req, error_response(400, "TARGET_REQUIRED", "target must be a string"));
        }
        const std::string target = trim_copy(body["target"].get<std::string>());
        if (target.empty()) {
            LOG_WARN("[data-dir] migration refused: TARGET_REQUIRED (empty)");
            return with_cors(req, error_response(400, "TARGET_REQUIRED", "target is required"));
        }
        // 每个拒绝分支都记一条 WARN:这些 409/400 曾经一个字都不落日志,用户反馈
        // 「提示程序占用」时无从判断是哪条分支、被谁占着。
        if (data_dir_migration->active()) {
            LOG_WARN("[data-dir] migration refused: MIGRATION_ACTIVE");
            return with_cors(req, error_response(
                409, "MIGRATION_ACTIVE", "a data directory migration is already running"));
        }
        std::unique_lock<std::shared_mutex> migration_lock(env::data_dir_write_mutex());
        if (deps.session_registry) {
            const auto busy = deps.session_registry->busy_session_ids();
            if (!busy.empty()) {
                std::string joined;
                for (const auto& id : busy) {
                    if (!joined.empty()) joined += ",";
                    joined += id;
                }
                LOG_WARN("[data-dir] migration refused: SESSIONS_BUSY sessions=" + joined);
                return with_cors(req, json_response(409, json{
                    {"error", "SESSIONS_BUSY"},
                    {"message", "a session is still running; wait for it to finish before migrating"},
                    {"busy_sessions", busy}}));
            }
        }
        {
            std::string holder;
            if (env::data_dir_has_other_daemons(resolve_data_dir(get_run_mode()), &holder)) {
                LOG_WARN("[data-dir] migration refused: OTHER_INSTANCES_ACTIVE " + holder);
                return with_cors(req, error_response(409, "OTHER_INSTANCES_ACTIVE", "close other ACECode instances before migrating"));
            }
        }
        if (deps.pty_registry) {
            std::string running;
            for (const auto& session : deps.pty_registry->list()) {
                if (session.status != "running") continue;
                if (!running.empty()) running += ",";
                running += session.id;
            }
            if (!running.empty()) {
                LOG_WARN("[data-dir] migration refused: CONSOLES_ACTIVE ptys=" + running);
                return with_cors(req, error_response(409, "CONSOLES_ACTIVE", "close console tabs before migrating"));
            }
        }
        const std::string current_dir = resolve_data_dir(get_run_mode());
        const std::string default_dir = resolve_default_data_dir(get_run_mode());
        const auto check = env::validate_migration_target(current_dir, target);
        if (check.error != env::MigrationTargetError::None) {
            LOG_WARN(std::string("[data-dir] migration refused: ") +
                     env::migration_target_error_code(check.error) + " message=" + check.message +
                     " target=" + target);
            return with_cors(req, json_response(400, json{
                {"error", env::migration_target_error_code(check.error)},
                {"message", check.message},
                {"target", target}}));
        }
        std::string error;
        if (!data_dir_migration->start(current_dir, default_dir, check.normalized_target, &error,
                deps.before_data_dir_copy, deps.on_data_dir_copy_failure)) {
            LOG_WARN("[data-dir] migration refused: MIGRATION_ACTIVE " + error);
            return with_cors(req, error_response(409, "MIGRATION_ACTIVE", error));
        }
        LOG_INFO("[data-dir] migration started: target=" + check.normalized_target);
        return with_cors(req, json_response(202, migration_json_or_null()));
    });

    CROW_ROUTE(app, "/api/config/data-dir/migration").methods(crow::HTTPMethod::GET)
    ([this, migration_json_or_null](const crow::request& req) {
        if (auto rej = require_auth(req)) return std::move(*rej);
        const json progress = migration_json_or_null();
        if (progress.is_null()) {
            return with_cors(req, error_response(404, "MIGRATION_NOT_FOUND",
                                                 "no data directory migration has been started"));
        }
        return with_cors(req, json_response(200, progress));
    });

    CROW_ROUTE(app, "/api/config/data-dir/cleanup").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        if (auto rej = require_auth(req)) return std::move(*rej);
        json body;
        try { body = json::parse(req.body); }
        catch (const std::exception& e) {
            LOG_WARN("[data-dir] cleanup refused: BAD_JSON");
            return with_cors(req, error_response(400, "BAD_JSON", std::string("bad json: ") + e.what()));
        }
        if (!body.is_object() || !body.contains("action") || !body["action"].is_string()) {
            LOG_WARN("[data-dir] cleanup refused: INVALID_ACTION (not a string)");
            return with_cors(req, error_response(400, "INVALID_ACTION", "action must be a string"));
        }
        const std::string action = body["action"].get<std::string>();
        if (action != "delete" && action != "keep") {
            LOG_WARN("[data-dir] cleanup refused: INVALID_ACTION action=" + action);
            return with_cors(req, error_response(400, "INVALID_ACTION",
                                                 "action must be \"delete\" or \"keep\""));
        }
        const std::string default_dir = resolve_default_data_dir(get_run_mode());
        const auto status = env::data_dir_status(get_run_mode());
        if (action == "delete") {
            if (!status.redirect_active) {
                LOG_WARN("[data-dir] cleanup refused: NO_MIGRATION");
                return with_cors(req, error_response(
                    409, "NO_MIGRATION",
                    "the data directory has not been migrated; nothing to delete"));
            }
            if (status.previous_exists) {
                std::string holder;
                if (env::data_dir_has_other_daemons(status.previous_dir, &holder)) {
                    LOG_WARN("[data-dir] cleanup refused: SESSIONS_BUSY " + holder);
                    return with_cors(req, error_response(409, "SESSIONS_BUSY", "the previous workspace is still in use"));
                }
                const std::string err = env::cleanup_previous_data_dir(status.previous_dir, default_dir);
                if (!err.empty()) {
                    LOG_WARN("[data-dir] cleanup failed: CLEANUP_FAILED " + err);
                    return with_cors(req, error_response(500, "CLEANUP_FAILED", err));
                }
                LOG_INFO("[data-dir] previous data directory removed: " + status.previous_dir);
            }
        }
        const std::string ack = env::acknowledge_data_dir_cleanup(default_dir);
        if (!ack.empty()) {
            LOG_WARN("[data-dir] cleanup failed: PERSIST_FAILED " + ack);
            return with_cors(req, error_response(500, "PERSIST_FAILED", ack));
        }
        return with_cors(req, json_response(200, data_dir_status_json(
            env::data_dir_status(get_run_mode()))));
    });

    // ---- native dialogs ---------------------------------------------------

    CROW_ROUTE(app, "/api/dialog/pick-folder").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        if (auto rej = require_auth(req)) return std::move(*rej);
        if (!deps.native_folder_picker_enabled || !deps.native_folder_picker) {
            return with_cors(req, error_response(501, "PICKER_UNAVAILABLE",
                                                 "native folder picker unavailable"));
        }
        auto picked = deps.native_folder_picker();
        if (!picked || picked->empty()) {
            return with_cors(req, json_response(200, json(nullptr)));
        }
        return with_cors(req, json_response(200, json{{"path", *picked}}));
    });

    CROW_ROUTE(app, "/api/dialog/pick-file").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        if (auto rej = require_auth(req)) return std::move(*rej);
        if (!deps.native_folder_picker_enabled || !deps.native_open_file_picker) {
            return with_cors(req, error_response(501, "PICKER_UNAVAILABLE",
                                                 "native file picker unavailable"));
        }
        const auto picked = deps.native_open_file_picker();
        if (!picked.error.empty()) {
            return with_cors(req, error_response(501, "PICKER_UNAVAILABLE", picked.error));
        }
        if (!picked.path || picked.path->empty()) {
            return with_cors(req, json_response(200, json(nullptr)));
        }
        return with_cors(req, json_response(200, json{{"path", *picked.path}}));
    });
}

}  // namespace acecode::web
