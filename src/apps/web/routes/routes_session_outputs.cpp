#include "web/server_impl.hpp"
#include "session_host/session_registry.hpp"
#include "web/handlers/session_history_handler.hpp"

namespace acecode::web {
using nlohmann::json;

// GET /api/sessions/:id/outputs[?workspace=<hash>]
// 会话的产物:这次对话生成的文档 / 表格 / 图片等文件,摘要面板的「产物」分组用。
// 列表由会话记录推导(session/session_outputs.hpp),已经不在磁盘上的不返回。
void WebServer::Impl::register_session_outputs() {
    CROW_ROUTE(app, "/api/sessions/<string>/outputs").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/sessions/<string>/outputs").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);

        SessionOutputsQuery query;
        query.session_id = id;
        if (deps.session_registry) {
            if (auto entry = deps.session_registry->acquire(id)) {
                // entry->cwd 决定记录存在哪;进了 worktree 的会话实际在 loop->cwd() 干活。
                query.project_dir = SessionStorage::get_project_dir(entry->cwd);
                query.cwd = entry->cwd;
                query.folders.push_back(entry->cwd);
                if (entry->loop) {
                    const std::string working_cwd = entry->loop->cwd();
                    if (!working_cwd.empty()) {
                        query.cwd = working_cwd;
                        query.folders.push_back(working_cwd);
                    }
                    for (auto& folder : entry->loop->workspace_extra_folders()) {
                        query.folders.push_back(std::move(folder));
                    }
                }
            }
        }
        if (query.project_dir.empty()) {
            // 还没恢复到内存的会话:按「工作区提示 → daemon 自己的目录 → 无工作区
            // 会话」的顺序找记录文件,最后一步最贵,放最后(同读记录接口)。
            std::vector<std::string> candidates;
            if (const char* hint = req.url_params.get("workspace")) {
                if (auto workspace = resolve_workspace(hint)) candidates.push_back(workspace->cwd);
            }
            candidates.push_back(deps.cwd);
            for (const auto& cwd : candidates) {
                if (cwd.empty()) continue;
                const std::string project_dir = SessionStorage::get_project_dir(cwd);
                if (session_history_file_in(project_dir, id).empty()) continue;
                query.project_dir = project_dir;
                query.cwd = cwd;
                break;
            }
            if (query.project_dir.empty()) {
                const auto meta = find_no_workspace_session_meta(id);
                if (meta && !meta->cwd.empty()) {
                    query.project_dir = SessionStorage::get_project_dir(meta->cwd);
                    query.cwd = meta->cwd;
                }
            }
            if (!query.cwd.empty()) query.folders.push_back(query.cwd);
        }

        json outputs = json::array();
        bool has_more = false;
        if (!query.project_dir.empty()) {
            const SessionOutputsResult result = session_outputs_index.query(query);
            has_more = result.has_more;
            for (const auto& file : result.files) {
                outputs.push_back({
                    {"path", file.path},
                    {"relative", file.relative},
                    {"source", session_output_source_name(file.source)},
                    {"size", file.size},
                    {"modified_ms", file.modified_ms},
                });
            }
        }
        // 路径来自磁盘上的文件名,不保证是合法 UTF-8;dump 用 replace 兜底,
        // 一个坏字节不该让整个接口变成 500。
        crow::response response(200);
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        response.body = json{{"session_id", id}, {"outputs", std::move(outputs)}, {"has_more", has_more}}
            .dump(-1, ' ', false, json::error_handler_t::replace);
        return with_cors(req, std::move(response));
    });
}

} // namespace acecode::web
