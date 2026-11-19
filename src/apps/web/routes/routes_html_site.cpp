#include "web/server_impl.hpp"
#include "web/handlers/html_site_handler.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

namespace acecode::web {
using nlohmann::json;

namespace {

constexpr std::uint64_t kMaxHtmlSiteFileBytes = 64ull * 1024 * 1024;

crow::response html_site_plain(int status) {
    crow::response r(status);
    r.add_header("Cache-Control", "no-store");
    r.add_header("X-Content-Type-Options", "nosniff");
    return r;
}

} // namespace

// 文件详情预览 .html 的页面资源站点(docs/html-preview.md、docs/daemon-api.md)。
//   POST /api/files/html-site {cwd,path} → {id, prefix, entry}:已鉴权,登记根目录;
//   GET  /api/html-site/<id>/<相对路径>:不走 require_auth,站点号即凭证(iframe 子资源
//        请求带不了 token),由 register_static 的 serve_path 分流进来。
void WebServer::Impl::register_html_site() {
    CROW_ROUTE(app, "/api/files/html-site").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });

    CROW_ROUTE(app, "/api/files/html-site").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        const auto respond = [this, &req](int status, const json& body) {
            crow::response r(status, body.dump());
            r.add_header("Content-Type", "application/json");
            r.add_header("Cache-Control", "no-store");
            return with_cors(req, std::move(r));
        };
        const auto body = json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("cwd") || !body["cwd"].is_string() ||
            !body.contains("path") || !body["path"].is_string()) {
            return respond(400, {{"error", "cwd and path required"}});
        }
        const std::string cwd = body["cwd"].get<std::string>();
        const std::string path = body["path"].get<std::string>();
        if (cwd.empty() || path.empty()) return respond(400, {{"error", "cwd and path required"}});

        const auto resolved = resolve_preview_path(cwd, path);
        if (std::holds_alternative<FileError>(resolved)) {
            return respond(400, {{"error", "invalid path"}});
        }
        const auto file = std::get<std::filesystem::path>(resolved);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec) || ec) {
            return respond(404, {{"error", "not found"}});
        }
        const auto mime = html_site_mime(path_to_utf8(file.filename()));
        if (!mime || mime->rfind("text/html", 0) != 0) {
            return respond(415, {{"error", "not an html file"}});
        }

        // 文件落在已登记的工作区内就以工作区为根(页面常引用 ../assets),否则只开放所在目录。
        std::optional<std::filesystem::path> workspace_root;
        const auto within = validate_path_within(cwd, path, allowed_file_cwds());
        if (std::holds_alternative<std::filesystem::path>(within)) {
            workspace_root = path_from_utf8(cwd);
        }
        const auto plan = plan_html_site(file, workspace_root);
        if (!plan) return respond(400, {{"error", "invalid path"}});
        const std::string id = html_sites.register_root(plan->root);
        return respond(200, {
            {"id", id},
            {"prefix", "/api/html-site/" + id + "/"},
            {"entry", encode_html_site_path(plan->entry)},
        });
    });
}

crow::response WebServer::Impl::serve_html_site(const crow::request& req) {
    // 与 auth_result_for_request 同一道门:远程访问没开时,非本机来源一律拒绝。
    if (!is_trusted_local_client_address(req.remote_ip_address)) {
        std::shared_lock<std::shared_mutex> lock(app_config_mu);
        const bool remote_enabled = deps.app_config ? deps.app_config->web.remote_enabled
            : (deps.web_cfg && deps.web_cfg->remote_enabled);
        if (!remote_enabled) return html_site_plain(403);
    }
    const auto parts = split_html_site_url(req.url);
    if (!parts || !is_html_site_id(parts->id)) return html_site_plain(404);
    const auto root = html_sites.lookup(parts->id);
    if (!root) return html_site_plain(404);
    const auto relative = decode_html_site_path(parts->rest);
    if (!relative) return html_site_plain(400);
    const auto mime = html_site_mime(*relative);
    if (!mime) return html_site_plain(404);
    const auto file = resolve_html_site_file(*root, *relative);
    if (!file) return html_site_plain(404);

    std::error_code ec;
    if (!std::filesystem::is_regular_file(*file, ec) || ec) return html_site_plain(404);
    const auto size = std::filesystem::file_size(*file, ec);
    if (ec) return html_site_plain(500);
    if (size > kMaxHtmlSiteFileBytes) return html_site_plain(413);
    std::ifstream in(*file, std::ios::binary);
    if (!in) return html_site_plain(500);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) return html_site_plain(500);

    crow::response r(200);
    r.body = std::move(bytes);
    r.add_header("Content-Type", *mime);
    r.add_header("Cache-Control", "no-cache");
    r.add_header("X-Content-Type-Options", "nosniff");
    r.add_header("Referrer-Policy", "no-referrer");
    if (html_site_mime_is_document(*mime)) {
        r.add_header("Content-Security-Policy", "sandbox allow-scripts allow-modals");
    }
    if (html_site_allows_cors(req.get_header_value("Sec-Fetch-Dest"))) {
        r.add_header("Access-Control-Allow-Origin", "*");
    }
    return r;
}

} // namespace acecode::web
