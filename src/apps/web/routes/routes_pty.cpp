// routes_pty.cpp — Route registrations extracted from server.cpp
#include "web/server_impl.hpp"

#include "environment/shell_command_line.hpp"
#include "environment/terminal_resolver.hpp"
#include "environment/terminal_runtime.hpp"

#include <filesystem>

namespace acecode::web {

using nlohmann::json;

// =====================================================================
// PTY-specific helpers
// =====================================================================

std::optional<crow::response> WebServer::Impl::require_pty_access(const crow::request& req) {
    if (!deps.pty_registry) {
        crow::response r(503);
        r.add_header("Content-Type", "application/json");
        r.body = json{{"error", "console is not available"}}.dump();
        add_cors(req, r);
        return r;
    }
    if (!is_loopback_address(req.remote_ip_address)) {
        log_unauthorized(req.url, req.remote_ip_address, "pty non-loopback");
        crow::response r(403);
        r.add_header("Content-Type", "application/json");
        r.body = json{{"error", "console is loopback-only"}}.dump();
        add_cors(req, r);
        return r;
    }
    return require_auth(req);
}

static json pty_info_json(const PtySessionInfo& info) {
    json j{
        {"id", info.id},
        {"title", info.title},
        {"shell", info.shell},
        {"cwd", info.cwd},
        {"owner_id", info.owner_id},
        {"status", info.status},
        {"pid", info.pid},
        {"backend", pty_backend_kind_name(info.backend)},
    };
    if (info.status == "exited") j["exit_code"] = info.exit_code;
    return j;
}

json WebServer::Impl::console_shells_payload() {
    json arr = json::array();
    ShellPaths shell_paths;
    std::string configured_default;
    if (deps.app_config) {
        shell_paths = deps.app_config->console.shell_paths;
        configured_default = deps.app_config->console.default_shell;
    }
    // 启动探测的快照(openspec: agent-default-terminal):默认 id 与 usable 以它为准,
    // 没探测过(测试 / 未 bootstrap)时退回目录层面的判定。
    const auto last = acecode::environment::terminal().last();
    std::string default_id = default_console_shell_id(configured_default, shell_paths);
    if (last && last->resolved.usable) default_id = last->resolved.id;
    for (const auto& opt : detect_console_shells(shell_paths)) {
        json item{{"id", opt.id},
                  {"label", opt.label},
                  {"available", opt.available},
                  {"needs_path", opt.needs_path},
                  {"path", opt.program},
                  {"configured_path", opt.configured_path}};
        bool usable = opt.available;
        bool probed = false;
        std::string probe_error;
        if (last) {
            for (const auto& c : last->candidates) {
                if (c.id != opt.id) continue;
                probed = c.probed;
                if (c.probed) usable = c.usable;
                if (!c.program.empty()) item["path"] = c.program;
                probe_error = c.probe_error;
                break;
            }
        }
        item["usable"] = usable;
        item["probed"] = probed;
        item["probe_error"] = probe_error;
        arr.push_back(item);
    }
    json out{{"shells", arr}, {"default", default_id}};
    if (last) {
        out["resolved"] = json{
            {"id", last->resolved.id},
            {"family", acecode::environment::terminal_family_name(last->resolved.family)},
            {"program", last->resolved.program},
            {"console_command", last->resolved.console_command},
            {"usable", last->resolved.usable},
            {"fallback_reason", last->resolved.fallback_reason},
        };
    }
    return out;
}

struct PtyWsState {
    std::string id;
    std::int64_t cursor = -1;
};

void WebServer::Impl::register_pty() {
        CROW_ROUTE(app, "/api/pty").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) { return cors_preflight(req); });
        CROW_ROUTE(app, "/api/pty/<string>").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });
        CROW_ROUTE(app, "/api/pty/<string>/resize").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });
        CROW_ROUTE(app, "/api/pty/<string>/title").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });
        CROW_ROUTE(app, "/api/pty/shells").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) { return cors_preflight(req); });
        CROW_ROUTE(app, "/api/console/config").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) { return cors_preflight(req); });

        // GET /api/pty/shells → { shells:[{id,label,available,needs_path}], default }
        CROW_ROUTE(app, "/api/pty/shells").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_pty_access(req)) return std::move(*rej);
            std::shared_lock<std::shared_mutex> config_lock(app_config_mu);
            crow::response r(console_shells_payload().dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // POST /api/pty {cwd?, title?, shell?, owner_id?} → 201 session info
        // shell = shell id(powershell/git-bash/cmd/...);省略 → 默认。id 不可用
        // (git-bash 需指定路径)→ 400 {error, shell, needs_path}。
        CROW_ROUTE(app, "/api/pty").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req) {
            if (auto rej = require_pty_access(req)) return std::move(*rej);
            std::string cwd_override, title, shell_id, owner_id;
            if (!req.body.empty()) {
                try {
                    auto body = json::parse(req.body);
                    cwd_override = body.value("cwd", "");
                    title = body.value("title", "");
                    shell_id = body.value("shell", "");
                    owner_id = body.value("owner_id", "");
                } catch (...) {
                    return with_cors(req, crow::response(400, "bad json"));
                }
            }
            if (owner_id.size() > 512) return with_cors(req, crow::response(400, "owner_id too long"));
            std::string shell_override;
            if (!shell_id.empty()) {
                ConsoleConfig console;
                if (deps.app_config) {
                    std::shared_lock<std::shared_mutex> config_lock(app_config_mu);
                    console = deps.app_config->console;
                }
                console.default_shell = shell_id;
                const auto resolution = acecode::environment::resolve_terminal(console);
                if (!resolution.resolved.usable || resolution.resolved.id != shell_id) {
                    json e{{"error", "shell unavailable"}, {"shell", shell_id}};
                    if (shell_id == "git-bash") e["needs_path"] = true;
                    crow::response r(400, e.dump());
                    r.add_header("Content-Type", "application/json");
                    return with_cors(req, std::move(r));
                }
                shell_override = resolution.resolved.console_command;
            } else if (const auto last = acecode::environment::terminal().last(); last && !last->resolved.usable) {
                return with_cors(req, crow::response(400, "no usable terminal; check Settings > Configuration"));
            }
            std::string error;
            auto info = deps.pty_registry->create(cwd_override, title, shell_override, error, owner_id);
            if (!info) {
                int code = error.find("limit") != std::string::npos ? 429 : 500;
                crow::response r(code);
                r.add_header("Content-Type", "application/json");
                r.body = json{{"error", error}}.dump();
                return with_cors(req, std::move(r));
            }
            crow::response r(201, pty_info_json(*info).dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // GET /api/pty → {backend, sessions: [...]}
        CROW_ROUTE(app, "/api/pty").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_pty_access(req)) return std::move(*rej);
            json arr = json::array();
            const char* owner = req.url_params.get("owner_id");
            const auto filter = owner ? std::optional<std::string>(owner) : std::nullopt;
            for (const auto& info : deps.pty_registry->list(filter)) {
                arr.push_back(pty_info_json(info));
            }
            json out{{"backend", pty_backend_kind_name(deps.pty_registry->backend())},
                     {"sessions", arr}};
            crow::response r(out.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        CROW_ROUTE(app, "/api/pty/transfer-owner").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req) {
            if (auto rej = require_pty_access(req)) return std::move(*rej);
            std::string from, to;
            try {
                const auto body = json::parse(req.body);
                from = body.at("from_owner").get<std::string>();
                to = body.at("to_owner").get<std::string>();
            } catch (...) {
                return with_cors(req, crow::response(400, "bad json"));
            }
            if (!deps.pty_registry->transfer_owner(from, to)) {
                return with_cors(req, crow::response(409, "owner transfer conflict"));
            }
            return with_cors(req, crow::response(204));
        });

        // DELETE /api/pty/<id> → 204 / 404
        CROW_ROUTE(app, "/api/pty/<string>").methods(crow::HTTPMethod::Delete)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_pty_access(req)) return std::move(*rej);
            if (!deps.pty_registry->remove(id)) {
                return with_cors(req, crow::response(404));
            }
            return with_cors(req, crow::response(204));
        });

        // POST /api/pty/<id>/resize {cols, rows} → 204 / 404
        CROW_ROUTE(app, "/api/pty/<string>/resize").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_pty_access(req)) return std::move(*rej);
            int cols = 0, rows = 0;
            try {
                auto body = json::parse(req.body);
                cols = body.value("cols", 0);
                rows = body.value("rows", 0);
            } catch (...) {
                return with_cors(req, crow::response(400, "bad json"));
            }
            if (cols < 2 || cols > 1000 || rows < 2 || rows > 1000) {
                return with_cors(req, crow::response(400, "cols/rows out of range"));
            }
            if (!deps.pty_registry->resize(id, cols, rows)) {
                return with_cors(req, crow::response(404));
            }
            return with_cors(req, crow::response(204));
        });

        // POST /api/pty/<id>/title {title} → 204 / 404。终端内程序经 OSC
        // 设置的标题由前端 xterm onTitleChange 同步回来,刷新恢复不丢。
        CROW_ROUTE(app, "/api/pty/<string>/title").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req, const std::string& id) {
            if (auto rej = require_pty_access(req)) return std::move(*rej);
            std::string title;
            try {
                auto body = json::parse(req.body);
                title = body.value("title", "");
            } catch (...) {
                return with_cors(req, crow::response(400, "bad json"));
            }
            if (!deps.pty_registry->set_title(id, title)) {
                return with_cors(req, crow::response(404));
            }
            return with_cors(req, crow::response(204));
        });

        // Validate a complete draft before publishing either config or runtime state.
        CROW_ROUTE(app, "/api/console/config").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config) return with_cors(req, crow::response(503));
            auto fail = [&](int status, const std::string& message) {
                crow::response r(status, json{{"error", message}}.dump());
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            };
            json body = json::parse(req.body, nullptr, false);
            if (!body.is_object()) return fail(400, "body must be an object");
            for (const char* key : {"default_shell", "shell_path", "git_bash_path"}) {
                if (body.contains(key) && !body[key].is_string()) return fail(400, std::string(key) + " must be a string");
            }
            std::lock_guard<std::shared_mutex> lock(app_config_mu);
            AppConfig next = *deps.app_config;
            if (body.contains("default_shell")) next.console.default_shell = body["default_shell"].get<std::string>();
            const auto options = detect_console_shells(next.console.shell_paths);
            const auto type = next.console.default_shell;
            if (!type.empty() && std::none_of(options.begin(), options.end(), [&](const auto& o) { return o.id == type; })) {
                return fail(400, "unknown terminal type");
            }
            auto set_path = [&](const std::string& id, std::string path) -> std::string {
                const auto first = path.find_first_not_of(" \t\r\n");
                path = first == std::string::npos ? "" : path.substr(first, path.find_last_not_of(" \t\r\n") - first + 1);
                if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
                if (id.empty()) return "shell_path requires default_shell";
                if (path.empty()) { next.console.shell_paths.erase(id); return {}; }
                const auto native = path_from_utf8(path);
                std::error_code ec;
                if (!native.is_absolute() || !std::filesystem::is_regular_file(native, ec) || ec) return "shell path must be an existing absolute file path";
                if (id == "git-bash" && is_wsl_system32_bash(path)) return "WSL bash is not Git Bash";
                const auto probe = acecode::environment::default_launch_probe();
                const auto result = probe(path, acecode::environment::probe_arguments(acecode::environment::terminal_family_for_id(id)));
                if (!result.ok) return "shell path failed the launch check: " + result.error;
                next.console.shell_paths[id] = path;
                return {};
            };
            if (body.contains("git_bash_path")) {
                const auto error = set_path("git-bash", body["git_bash_path"].get<std::string>());
                if (!error.empty()) return fail(400, error);
            }
            if (body.contains("shell_path")) {
                const auto error = set_path(type, body["shell_path"].get<std::string>());
                if (!error.empty()) return fail(400, error);
            }
            const auto resolution = acecode::environment::resolve_terminal(next.console);
            if (!resolution.resolved.usable) return fail(400, resolution.resolved.fallback_reason);
            try {
                if (!deps.config_path.empty()) save_config(next, deps.config_path);
                else save_config(next);
            } catch (const std::exception& e) { return fail(500, e.what()); }
            deps.app_config->console = next.console;
            acecode::environment::terminal().publish(resolution);
            if (deps.pty_registry) deps.pty_registry->set_default_shell(resolution.resolved.console_command);
            crow::response r(200, console_shells_payload().dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // WS /ws/pty/<id>?cursor=N — 原始字节直传 + 0x00 控制帧。
        CROW_WEBSOCKET_ROUTE(app, "/ws/pty/<string>")
            .onaccept([this](const crow::request& req, void** userdata) -> bool {
                if (!deps.pty_registry) return false;
                if (!is_loopback_address(req.remote_ip_address)) {
                    log_unauthorized(req.url, req.remote_ip_address, "pty ws non-loopback");
                    return false;
                }
                // Crow 的 WS onopen 拿不到 route 参数,从 url 解出 id 存
                // userdata("/ws/pty/<id>",query 已被 Crow 剥离到 url_params)。
                std::string path = req.url;
                const std::string prefix = "/ws/pty/";
                auto pos = path.find(prefix);
                if (pos == std::string::npos) return false;
                std::string id = path.substr(pos + prefix.size());
                if (auto qpos = id.find('?'); qpos != std::string::npos) {
                    id = id.substr(0, qpos);
                }
                if (id.empty()) return false;
                auto* state = new PtyWsState();
                state->id = id;
                if (auto c = req.url_params.get("cursor")) {
                    try { state->cursor = std::stoll(c); } catch (...) {}
                }
                *userdata = state;
                return true;
            })
            .onopen([this](crow::websocket::connection& conn) {
                auto* state = static_cast<PtyWsState*>(conn.userdata());
                if (!state) { conn.close("no state"); return; }
                bool ok = deps.pty_registry->connect(
                    state->id, &conn, state->cursor,
                    [&conn](const std::string& frame) {
                        conn.send_binary(frame);
                    });
                if (!ok) {
                    conn.close("unknown pty session");
                }
            })
            .onmessage([this](crow::websocket::connection& conn,
                              const std::string& data, bool /*is_binary*/) {
                auto* state = static_cast<PtyWsState*>(conn.userdata());
                if (!state) return;
                deps.pty_registry->write_input(state->id, data);
            })
            .onclose([this](crow::websocket::connection& conn,
                            const std::string& /*reason*/, uint16_t /*code*/) {
                auto* state = static_cast<PtyWsState*>(conn.userdata());
                if (!state) return;
                if (deps.pty_registry) {
                    deps.pty_registry->disconnect(state->id, &conn);
                }
                conn.userdata(nullptr);
                delete state;
            });
    }
} // namespace acecode::web
