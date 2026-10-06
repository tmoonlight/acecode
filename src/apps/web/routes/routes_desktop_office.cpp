#include "web/server_impl.hpp"
#include "session/session_recent_activity.hpp"

namespace acecode::web {
using nlohmann::json;
namespace {
json office_item(const json& source) {
    json item = json::object();
    for (const auto* key : {"id", "active", "status", "workspace_hash", "workspace_name",
             "parent_session_id", "agent_path", "archived", "no_workspace", "title", "summary",
             "last_user_message_at", "last_turn_outcome", "updated_at", "context_window", "token_usage"}) {
        if (source.contains(key)) item[key] = source[key];
    }
    for (const auto* key : {"title", "summary"}) {
        if (item.contains(key) && item[key].is_string())
            item[key] = truncate_utf8_prefix(item[key].get_ref<const std::string&>(), 240);
    }
    return item;
}
} // namespace

void WebServer::Impl::register_desktop_office() {
    CROW_ROUTE(app, "/api/desktop-office").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return with_cors(req, crow::response(204)); });

    CROW_ROUTE(app, "/api/desktop-office").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        const auto respond = [&](int status, const json& body) {
            crow::response response(status, body.dump());
            response.add_header("Content-Type", "application/json");
            response.add_header("Cache-Control", "no-store");
            return with_cors(req, std::move(response));
        };
        const auto recent = global_session_search->recent_user_sessions(5);
        json offices = json::array();
        for (const auto& entry : recent.entries) {
            auto item = entry.active ? session_info_to_json(*entry.active, &entry.meta)
                                     : session_meta_to_json(entry.meta, entry.workspace_hash);
            item["last_user_message_at"] = entry.meta.last_user_message_at;
            item["last_turn_outcome"] = entry.meta.last_turn_outcome;
            item["workspace_name"] = entry.workspace_name;
            offices.push_back(office_item(item));
        }
        std::string selected = req.url_params.get("session") ? req.url_params.get("session") : "";
        std::string workspace = req.url_params.get("workspace") ? req.url_params.get("workspace") : "";
        if (selected.empty() && !offices.empty()) {
            selected = offices.front().value("id", std::string{});
            workspace = offices.front().value("workspace_hash", std::string{});
        }
        json result{{"offices", offices}, {"selected", nullptr}, {"agents", json::array()},
                    {"complete", recent.progress.complete}, {"connected", true}};
        if (selected.empty()) return respond(200, result);
        const auto active = deps.session_client ? deps.session_client->list_sessions()
                                                : std::vector<SessionInfo>{};
        // A child selected in the main window belongs to its root's office.
        for (const auto& session : active) {
            if (session.id == selected && !session.parent_session_id.empty()) {
                selected = session.parent_session_id;
                workspace = session.workspace_hash;
                break;
            }
        }
        auto ws = resolve_session_workspace(selected, workspace);
        // Global discovery also includes workspaces that have no visible sidebar
        // registration. Their persisted cwd is still authoritative for storage.
        for (const auto& entry : recent.entries) {
            if (entry.meta.id != selected) continue;
            acecode::desktop::WorkspaceMeta source;
            source.hash = entry.meta.no_workspace ? std::string{} : entry.workspace_hash;
            source.cwd = entry.meta.cwd;
            source.name = entry.workspace_name;
            ws = std::move(source);
            break;
        }
        if (!ws) return respond(404, {{"error", "office session not found"}, {"offices", offices}});
        auto meta = find_session_meta_for_workspace(*ws, selected);
        if (!meta || meta->archived) {
            return respond(404, {{"error", "office session unavailable"}, {"offices", offices}});
        }
        if (!meta->parent_session_id.empty()) {
            selected = meta->parent_session_id;
            meta = find_session_meta_for_workspace(*ws, selected);
            if (!meta || meta->archived) return respond(404, {{"error", "office root unavailable"}});
        }
        if (meta->last_user_message_at.empty()) {
            const auto old = read_session_recent_activity(SessionStorage::session_path(
                SessionStorage::get_project_dir(meta->cwd), selected));
            meta->last_user_message_at = old.last_user_message_at;
            if (meta->last_turn_outcome.empty()) meta->last_turn_outcome = old.last_turn_outcome;
        }
        auto root = session_meta_to_json(*meta, ws->hash);
        const auto decorate = [&](const json& source, const std::string& id) {
            auto item = office_item(source);
            if (deps.session_registry) {
                if (const auto entry = deps.session_registry->acquire(id); entry && entry->loop) {
                    item["activity"] = entry->loop->events().activity_snapshot();
                }
            }
            return item;
        };
        for (const auto& session : active) {
            if (session.id == selected) root = session_info_to_json(session, &*meta);
        }
        root = decorate(std::move(root), selected);
        result["selected"] = root;
        result["agents"].push_back(root);
        for (const auto& child : active) {
            if (child.parent_session_id != selected) continue;
            result["agents"].push_back(decorate(session_info_to_json(child, nullptr), child.id));
        }
        return respond(200, result);
    });
}
} // namespace acecode::web
