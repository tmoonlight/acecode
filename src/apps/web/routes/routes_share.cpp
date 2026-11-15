#include "web/server_impl.hpp"
#include "share/share_projection.hpp"
#include "utils/sha1.hpp"
#include "session_host/thread_service.hpp"
#include "config/settings_mutations.hpp"
#include "web/handlers/session_history_handler.hpp"

namespace acecode::web {
using nlohmann::json;
namespace {
crow::response response_json(const crow::request& req, int status, const json& body) {
    crow::response response(status, body.dump());
    add_loopback_cors_headers(req, response);
    response.set_header("Content-Type", "application/json");
    response.set_header("Cache-Control", "no-store");
    return response;
}
} // namespace

void WebServer::Impl::register_sharing() {
    CROW_ROUTE(app, "/api/config/desktop-clipboard").methods(crow::HTTPMethod::GET, crow::HTTPMethod::PUT, crow::HTTPMethod::Options)
    ([this](const crow::request& req) {
        if (req.method == crow::HTTPMethod::Options) return cors_preflight(req);
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (!deps.app_config) return response_json(req, 503, {{"error", "config_unavailable"}});
        std::lock_guard<std::shared_mutex> lock(app_config_mu);
        if (req.method == crow::HTTPMethod::PUT) {
            const auto body = json::parse(req.body, nullptr, false);
            if (!body.is_object() || !body.contains("enabled") || !body["enabled"].is_boolean())
                return response_json(req, 400, {{"error", "invalid_preference"}});
            SettingsMutationOptions options; options.config_path = deps.config_path; options.live_config = deps.app_config;
            const auto result = set_invite_clipboard_detection(body["enabled"].get<bool>(), options);
            if (!result.ok) return response_json(req, 500, {{"error", "save_failed"}});
        }
        return response_json(req, 200, {{"ok", true}, {"enabled", deps.app_config->desktop.detect_invite_links_in_clipboard}});
    });
    CROW_ROUTE(app, "/api/sessions/<string>/shares").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req, const std::string& id) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (deps.dangerous) return response_json(req,403, {{"error", "dangerous_mode"}});
        if (!deps.session_registry || !deps.remote_web_proxy)
            return response_json(req,503, {{"error", "sharing_unavailable"}});
        auto body = json::parse(req.body, nullptr, false);
        if (!body.is_object()) return response_json(req,400, {{"error", "invalid_request"}});
        if ((body.contains("mode") && !body["mode"].is_string()) ||
            (body.contains("visibility") && !body["visibility"].is_string()))
            return response_json(req, 400, {{"error", "invalid_options"}});
        auto workspace = resolve_session_workspace(id);
        if (!workspace) return response_json(req,404, {{"error", "session_not_found"}});
        auto meta = find_session_meta_for_workspace(*workspace, id);
        if (!meta) return response_json(req,404, {{"error", "session_not_found"}});
        const auto mode = body.value("mode", std::string("interactive"));
        const auto visibility = body.value("visibility", std::string("conversation"));
        if ((mode != "read_only" && mode != "interactive") ||
            (visibility != "conversation" && visibility != "tools") ||
            (body.contains("expires_in") && !body["expires_in"].is_number_integer()))
            return response_json(req,400, {{"error", "invalid_options"}});
        const auto duration = body.value("expires_in", std::int64_t{86400});
        if (duration < 60 || duration > 30 * 86400) return response_json(req,400, {{"error", "invalid_expiry"}});
        share::ShareRecord record;
        record.session_id = id; record.cwd = meta->cwd.empty() ? workspace->cwd : meta->cwd;
        record.owner = (deps.remote_web_computer_name ? deps.remote_web_computer_name()
            : discover_remote_web_computer_name()).value_or("ACECode");
        record.title = meta->title; record.include_tools = visibility == "tools";
        record.allow_questions = mode == "interactive";
        record.expires_at = now_unix_ms() / 1000 + duration;
        if (!acquire_shared_session(record)) return response_json(req, 409, {{"error", "session_unavailable"}});
        // Validate before persisting; long titles cannot create unusable shares.
        if (record.title.size() > 512) return response_json(req,400, {{"error", "title_too_long"}});
        record = shares->create(std::move(record), now_unix_ms() / 1000);
        try {
            reconcile_share_listener();
            auto result = share::share_public_json(record);
            result["link"] = share_link(record);
            return response_json(req,201, result);
        } catch (const std::exception&) {
            shares->revoke(record.id); reconcile_share_listener();
            return response_json(req,503, {{"error", "share_listener_unavailable"}});
        }
    });

    CROW_ROUTE(app, "/api/shares").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        auto result = json::array();
        const auto now = now_unix_ms() / 1000;
        for (const auto& record : shares->list()) {
            auto item = share::share_public_json(record);
            item["active"] = !record.revoked && record.expires_at > now;
            if (item["active"] == true) {
                try { item["link"] = share_link(record); }
                catch (const std::exception&) { item["error"] = "share_listener_unavailable"; }
            }
            result.push_back(std::move(item));
        }
        return response_json(req,200, {{"shares", result}});
    });

    CROW_ROUTE(app, "/api/shares/<string>").methods(crow::HTTPMethod::DELETE)
    ([this](const crow::request& req, const std::string& id) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (!shares->revoke(id)) return response_json(req,404, {{"error", "unknown_share"}});
        expire_share_connections(); reconcile_share_listener();
        return response_json(req,200, {{"ok", true}});
    });

    CROW_ROUTE(app, "/api/shares/<string>/asks").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req, const std::string& id) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (!shares->find(id)) return response_json(req,404, {{"error", "unknown_share"}});
        return response_json(req,200, {{"asks", shares->questions(id)}});
    });

    CROW_ROUTE(app, "/api/share/v1/hello").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        share::ShareRecord record;
        if (auto denied = require_share_auth(req, record)) return std::move(*denied);
        if (!acquire_shared_session(record)) return response_json(req,404, {{"error", "session_unavailable"}});
        const auto now = now_unix_ms() / 1000;
        return response_json(req,200, share::redact_share_json({{"version", 1},
            {"identity", sha1_hex(record.owner + projects_dir())}, {"session_id", record.session_id},
            {"owner", record.owner}, {"title", record.title}, {"include_tools", record.include_tools},
            {"allow_questions", record.allow_questions}, {"expires_at", record.expires_at},
            {"remaining", 30 - (record.question_day == now / 86400 ? record.question_count : 0)},
            {"resets_at", (now / 86400 + 1) * 86400}}));
    });

    CROW_ROUTE(app, "/api/share/v1/messages").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req) {
        share::ShareRecord record;
        if (auto denied = require_share_auth(req, record)) return std::move(*denied);
        try {
            const auto requested_limit = req.url_params.get("limit");
            const auto history = parse_history_request(requested_limit ? requested_limit : "100",
                req.url_params.get("before"), nullptr, nullptr, nullptr, 0);
            if (!history.error.empty()) return response_json(req, 400, {{"error", "invalid_cursor"}});
            return response_json(req,200, shared_messages(record, history.request));
        } catch (const std::exception&) { return response_json(req,400, {{"error", "invalid_history_request"}}); }
    });

    CROW_ROUTE(app, "/api/share/v1/turns").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req) {
        share::ShareRecord record;
        if (auto denied = require_share_auth(req, record)) return std::move(*denied);
        const auto entry = acquire_shared_session(record);
        if (!entry) return response_json(req,404, {{"error", "session_unavailable"}});
        ThreadService service({deps.session_registry, deps.session_client});
        ThreadScope scope{record.cwd, record.session_id, entry->sm.get(), entry->no_workspace};
        const auto cursor = req.url_params.get("cursor");
        const auto result = service.read(scope, record.session_id, cursor ? cursor : "", 8, record.include_tools, 2000);
        if (!result.success) return response_json(req,400, {{"error", "invalid_cursor"}});
        return response_json(req,200, {{"source", "external_untrusted"}, {"session", share::redact_share_json(result.value)}});
    });

    CROW_ROUTE(app, "/api/share/v1/attachments/<string>/blob").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req, const std::string& id) {
        share::ShareRecord record;
        if (auto denied = require_share_auth(req, record)) return std::move(*denied);
        if (!is_valid_attachment_id(id)) return response_json(req,404, {{"error", "attachment_not_found"}});
        const auto session = acquire_shared_session(record);
        bool visible = false;
        if (session && session->sm) for (const auto& message : session->sm->load_active_messages()) {
            const auto projected = share::project_message(chat_message_to_json(message), record.include_tools);
            if (!projected.is_object() || !projected.contains("content_parts")) continue;
            for (const auto& part : projected["content_parts"])
                if (part.value("type", "") == "image" && part.contains("attachment") &&
                    part["attachment"].value("id", "") == id) visible = true;
        }
        if (!visible) return response_json(req, 404, {{"error", "attachment_not_found"}});
        const auto attachment = load_attachment(SessionStorage::get_project_dir(record.cwd), record.session_id, id);
        if (!attachment || attachment->session_id != record.session_id ||
            (attachment->mime_type != "image/png" && attachment->mime_type != "image/jpeg" &&
             attachment->mime_type != "image/gif" && attachment->mime_type != "image/webp" && attachment->mime_type != "image/bmp"))
            return response_json(req,404, {{"error", "attachment_not_found"}});
        const auto bytes = read_attachment_bytes(*attachment);
        if (!bytes) return response_json(req,404, {{"error", "attachment_not_found"}});
        crow::response response(200, *bytes);
        response.set_header("Content-Type", attachment->mime_type);
        response.set_header("X-Content-Type-Options", "nosniff");
        response.set_header("Cache-Control", "no-store");
        return response;
    });
}
} // namespace acecode::web
