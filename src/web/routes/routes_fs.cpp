// routes_fs.cpp — /api/fs:Web「选择文件 / 文件夹」弹窗的服务端目录浏览接口
// (openspec add-web-path-picker)。与 /api/files 的语义差别见
// handlers/fs_browser_handler.hpp 头注释:已鉴权即可浏览任意绝对目录,只读,不做
// workspace 白名单;每次列举记一条带路径的日志。
#include "web/server_impl.hpp"
#include "web/handlers/fs_browser_handler.hpp"
#include "web/project_creation.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <string>

namespace acecode::web {

using nlohmann::json;

namespace {

json root_entry_to_json(const FsRootEntry& r) {
    json item{
        {"path", r.path},
        {"label", r.label},
        {"drive_type", r.drive_type},
    };
    if (r.total_bytes.has_value()) item["total_bytes"] = *r.total_bytes;
    if (r.free_bytes.has_value())  item["free_bytes"]  = *r.free_bytes;
    return item;
}

json browse_entry_to_json(const FsBrowseEntry& e) {
    json item{
        {"name", e.name},
        {"path", e.path},
        {"kind", e.kind},
        {"hidden", e.hidden},
    };
    if (e.size.has_value())        item["size"]        = *e.size;
    if (e.modified_ms.has_value()) item["modified_ms"] = *e.modified_ms;
    if (e.link_target.has_value()) item["link_target"] = *e.link_target;
    return item;
}

int browse_error_status(FsBrowseErrorKind kind) {
    switch (kind) {
        case FsBrowseErrorKind::NotAbsolute:  return 400;
        case FsBrowseErrorKind::NotFound:     return 404;
        case FsBrowseErrorKind::NotDirectory: return 404;
        case FsBrowseErrorKind::AccessDenied: return 403;
        case FsBrowseErrorKind::IoError:      return 500;
    }
    return 500;
}

const char* browse_error_code(FsBrowseErrorKind kind) {
    switch (kind) {
        case FsBrowseErrorKind::NotAbsolute:  return "path must be absolute";
        case FsBrowseErrorKind::NotFound:     return "not found";
        case FsBrowseErrorKind::NotDirectory: return "not a directory";
        case FsBrowseErrorKind::AccessDenied: return "permission denied";
        case FsBrowseErrorKind::IoError:      return "io error";
    }
    return "io error";
}

} // namespace

void WebServer::Impl::register_fs() {
    CROW_ROUTE(app, "/api/fs/roots").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) {
        return cors_preflight(req);
    });
    CROW_ROUTE(app, "/api/fs/list").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) {
        return cors_preflight(req);
    });

    // GET /api/fs/roots — 可浏览的根节点:盘符 / 文件系统根、主目录、桌面、默认项目
    // 父目录(存在时)、已注册工作区,以及主机名与操作系统类型。盘符标签文案由前端组合。
    CROW_ROUTE(app, "/api/fs/roots").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req) {
        if (auto rej = require_auth(req)) return std::move(*rej);

        json body;
        body["host"] = host_name();
        body["os"] = host_os_name();
        const std::string home = home_directory();
        body["home"] = home;

        json roots = json::array();
        for (const auto& r : enumerate_roots()) roots.push_back(root_entry_to_json(r));
        body["roots"] = std::move(roots);

        json quick = json::array();
        if (!home.empty()) quick.push_back(json{{"kind", "home"}, {"path", home}});
        if (const auto desktop = desktop_directory(home)) {
            quick.push_back(json{{"kind", "desktop"}, {"path", *desktop}});
        }
        {
            // 默认项目父目录是 create_project_directory 惰性创建的,还不存在就不给入口,
            // 否则用户点进去只会看到 404。
            const std::string projects_parent = default_project_parent_directory(projects_dir());
            std::error_code ec;
            if (!projects_parent.empty() &&
                std::filesystem::is_directory(path_from_utf8(projects_parent), ec) && !ec) {
                quick.push_back(json{{"kind", "projects"}, {"path", projects_parent}});
            }
        }
        body["quick"] = std::move(quick);

        json workspaces = json::array();
        if (deps.workspace_registry) {
            for (const auto& m : deps.workspace_registry->list()) {
                const auto normalized = normalize_browse_path(m.cwd);
                workspaces.push_back(json{
                    {"hash", m.hash},
                    {"name", m.name},
                    {"path", normalized ? *normalized : m.cwd},
                });
            }
        }
        body["workspaces"] = std::move(workspaces);

        crow::response r(200);
        r.body = body.dump();
        r.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(r));
    });

    // GET /api/fs/list?path=<abs>&show_hidden=<0|1> — 列 path 的直接子项。
    CROW_ROUTE(app, "/api/fs/list").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req) {
        if (auto rej = require_auth(req)) return std::move(*rej);

        std::string path_q;
        bool show_hidden = false;
        if (auto p = req.url_params.get("path")) path_q = p;
        if (auto s = req.url_params.get("show_hidden")) {
            const std::string v = s;
            show_hidden = (v == "1" || v == "true");
        }

        auto browsed = browse_directory(path_q, show_hidden);
        if (std::holds_alternative<FsBrowseError>(browsed)) {
            const auto& err = std::get<FsBrowseError>(browsed);
            crow::response r(browse_error_status(err.kind));
            json body{{"error", browse_error_code(err.kind)}};
            if (!err.message.empty()) body["detail"] = err.message;
            r.body = body.dump();
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        }

        const auto& listed = std::get<FsBrowseResult>(browsed);
        LOG_INFO("[web] fs list path=" + listed.path +
                 " entries=" + std::to_string(listed.entries.size()) +
                 (listed.truncated ? " truncated" : ""));

        json body{
            {"path", listed.path},
            {"parent", listed.parent},
            {"truncated", listed.truncated},
        };
        json entries = json::array();
        for (const auto& e : listed.entries) entries.push_back(browse_entry_to_json(e));
        body["entries"] = std::move(entries);

        crow::response r(200);
        r.body = body.dump();
        r.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(r));
    });
}

} // namespace acecode::web
