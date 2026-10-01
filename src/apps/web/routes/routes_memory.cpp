#include "web/server_impl.hpp"
#include "web/handlers/memory_handler.hpp"

#include "config/settings_mutations.hpp"
#include "session/session_storage.hpp"
#include "session_host/memory_runtime.hpp"
#include "session_host/memory_scheduler.hpp"

namespace acecode::web {
using nlohmann::json;

// 设置 > 个性化 > 记忆(openspec unify-memory-system)。
//   GET/PUT /api/config/memory              使用记忆 / 记忆摘要设置(config.json)
//   GET     /api/memory?workspace=<hash>    全局与该工作区的条目 + 记忆摘要状态
//   GET/PUT/DELETE /api/memory/<scope>/<name>?workspace=<hash>
//   POST    /api/memory/reset {scope, workspace}
// 工作区只能经已登记的 workspace hash 选择,客户端不能借此读写任意目录。编辑走
// 存储层的脱敏与写锁并记为手写(source: manual);删除记墓碑。
void WebServer::Impl::register_memory() {
    const auto respond = [this](const crow::request& req, int status, const json& body) {
        crow::response response(status);
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        response.body = body.dump();
        return with_cors(req, std::move(response));
    };
    // 返回 nullopt = 给了 hash 但不是已登记的工作区;空串 = 没有选工作区。
    const auto project_dir_of = [this](const std::string& hash) -> std::optional<std::string> {
        if (hash.empty()) return std::string{};
        const auto workspace = resolve_workspace(hash);
        if (!workspace) return std::nullopt;
        return SessionStorage::get_project_dir(workspace->cwd);
    };
    const auto query = [](const crow::request& req, const char* key) {
        const char* value = req.url_params.get(key);
        return std::string(value ? value : "");
    };

    CROW_ROUTE(app, "/api/config/memory").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/config/memory").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        std::shared_lock<std::shared_mutex> lock(app_config_mu);
        return respond(req, 200, memory_settings_json(deps.app_config->memory, deps.memory != nullptr));
    });

    CROW_ROUTE(app, "/api/config/memory").methods(crow::HTTPMethod::PUT)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        const auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) return respond(req, 400, {{"error", "BAD_JSON"}});
        std::lock_guard<std::shared_mutex> lock(app_config_mu);
        MemoryConfig next;
        std::string error;
        if (!parse_memory_settings_request(body, deps.app_config->memory, next, error)) {
            return respond(req, 400, {{"error", "BAD_REQUEST"}, {"message", error}});
        }
        SettingsMutationOptions options;
        options.config_path = deps.config_path;
        options.live_config = deps.app_config;
        const auto result = set_memory_settings(next, options);
        if (!result.ok) {
            const bool validation = result.error_kind == SettingsMutationErrorKind::Validation;
            return respond(req, validation ? 400 : 500,
                           {{"error", validation ? "BAD_REQUEST" : "PERSIST_FAILED"},
                            {"message", result.error}});
        }
        // 本进程立即生效;其他 ACECode 进程由调度器按 config.json 修改时间重读。
        if (deps.memory) deps.memory->update_config(deps.app_config->memory);
        return respond(req, 200, memory_settings_json(deps.app_config->memory, deps.memory != nullptr));
    });

    CROW_ROUTE(app, "/api/memory").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/memory").methods(crow::HTTPMethod::GET)
    ([this, respond, project_dir_of, query](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.memory) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        const auto project_dir = project_dir_of(query(req, "workspace"));
        if (!project_dir) return respond(req, 404, {{"error", "UNKNOWN_WORKSPACE"}});
        auto& memory = *deps.memory->service();
        return respond(req, 200, memory_overview_json(
            memory, *project_dir, read_memory_summary_status(memory, *project_dir)));
    });

    CROW_ROUTE(app, "/api/memory/reset").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/memory/reset").methods(crow::HTTPMethod::POST)
    ([this, respond, project_dir_of](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.memory) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        const auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) return respond(req, 400, {{"error", "BAD_JSON"}});
        const auto scope = parse_memory_scope(body.value("scope", std::string{}));
        if (!scope) return respond(req, 400, {{"error", "BAD_REQUEST"}, {"message", "unknown scope"}});
        const auto project_dir = project_dir_of(body.value("workspace", std::string{}));
        if (!project_dir) return respond(req, 404, {{"error", "UNKNOWN_WORKSPACE"}});
        std::string error;
        if (!deps.memory->service()->reset_scope(*scope, *project_dir, error)) {
            return respond(req, 400, {{"error", "RESET_FAILED"}, {"message", error}});
        }
        return respond(req, 200, {{"ok", true}});
    });

    CROW_ROUTE(app, "/api/memory/<string>/<string>").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&, const std::string&) {
        return cors_preflight(req);
    });

    CROW_ROUTE(app, "/api/memory/<string>/<string>")
        .methods(crow::HTTPMethod::GET, crow::HTTPMethod::PUT, crow::HTTPMethod::DELETE)
    ([this, respond, project_dir_of, query](const crow::request& req, const std::string& scope_name,
                                             const std::string& name) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.memory) return respond(req, 503, {{"error", "UNAVAILABLE"}});
        const auto scope = parse_memory_scope(scope_name);
        if (!scope) return respond(req, 400, {{"error", "BAD_REQUEST"}, {"message", "unknown scope"}});
        const auto project_dir = project_dir_of(query(req, "workspace"));
        if (!project_dir) return respond(req, 404, {{"error", "UNKNOWN_WORKSPACE"}});
        auto& memory = *deps.memory->service();
        auto registry = memory.scope(*scope, *project_dir);
        if (!registry) return respond(req, 400, {{"error", "NO_WORKSPACE"}});
        registry->reload();

        if (req.method == crow::HTTPMethod::DELETE) {
            std::string error;
            if (!memory.delete_entry(*scope, *project_dir, name, error)) {
                return respond(req, 404, {{"error", "NOT_FOUND"}, {"message", error}});
            }
            return respond(req, 200, {{"ok", true}});
        }
        const auto existing = registry->find(name);
        if (!existing) return respond(req, 404, {{"error", "NOT_FOUND"}});
        if (req.method == crow::HTTPMethod::GET) {
            return respond(req, 200, memory_entry_json(*scope, *existing, true));
        }

        const auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) return respond(req, 400, {{"error", "BAD_JSON"}});
        MemoryEntryEdit edit;
        std::string error;
        if (!parse_memory_entry_edit(body, edit, error)) {
            return respond(req, 400, {{"error", "BAD_REQUEST"}, {"message", error}});
        }
        MemoryWriteRequest request;
        request.name = name;
        request.type = edit.type.value_or(existing->type);
        request.description = edit.description;
        request.body = edit.body;
        request.mode = MemoryWriteMode::Update;
        request.source = kMemorySourceManual;  // 用户亲手改过的条目,整合不得再动
        MemoryWriteOutcome outcome;
        const auto written = registry->upsert(request, error, &outcome);
        if (!written) return respond(req, 400, {{"error", "WRITE_FAILED"}, {"message", error}});
        json out = memory_entry_json(*scope, *written, true);
        out["redactions"] = outcome.redactions;
        return respond(req, 200, out);
    });
}

} // namespace acecode::web
