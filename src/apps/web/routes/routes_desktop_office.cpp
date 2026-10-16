#include "web/server_impl.hpp"
#include "session/session_attention.hpp"
#include "session/session_recent_activity.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace acecode::web {
using nlohmann::json;
namespace {
json office_item(const json& source) {
    json item = json::object();
    for (const auto* key : {"id", "active", "status", "workspace_hash", "workspace_name",
             "parent_session_id", "agent_path", "expert_id", "expert_member_id", "expert",
             "archived", "no_workspace", "title", "summary", "attention_state",
             "last_user_message_at", "last_turn_outcome", "updated_at", "context_window", "token_usage"}) {
        if (source.contains(key)) item[key] = source[key];
    }
    for (const auto* key : {"title", "summary"}) {
        if (item.contains(key) && item[key].is_string())
            item[key] = truncate_utf8_prefix(item[key].get_ref<const std::string&>(), 240);
    }
    return item;
}
// A finished card shows the start of the reply. After a daemon restart the live
// activity no longer holds it, so read it backwards from the transcript; the file
// size keys the cache so the 3-second poll does not re-read unchanged files.
std::string cached_last_reply(const std::string& path) {
    static std::mutex mu;
    static std::unordered_map<std::string, std::pair<std::uintmax_t, std::string>> cache;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path_from_utf8(path), ec);
    if (ec) return {};
    {
        std::lock_guard<std::mutex> lock(mu);
        const auto hit = cache.find(path);
        if (hit != cache.end() && hit->second.first == size) return hit->second.second;
    }
    auto reply = read_session_last_reply(path);
    std::lock_guard<std::mutex> lock(mu);
    if (cache.size() > 256) cache.clear();
    cache[path] = {size, reply};
    return reply;
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
        const auto active = deps.session_client ? deps.session_client->list_sessions()
                                                : std::vector<SessionInfo>{};
        // Companion pets show every main session that is working, waiting for the
        // user, or finished with output nobody has read yet (Codex Mini's session
        // cards). The busy flag and the cached read/unread record filter first, so
        // only those few sessions pay for full serialization.
        json activities = json::array();
        std::unordered_set<std::string> listed;
        const auto add_activity = [&](json item, const std::string& id) {
            auto card = office_item(item);
            json activity = json::object();
            if (deps.session_registry) {
                if (const auto entry = deps.session_registry->acquire(id); entry && entry->loop) {
                    activity = entry->loop->events().activity_snapshot();
                }
            }
            const bool running = item.value("status", std::string{}) == "running";
            const auto path = item.value("session_path", std::string{});
            if (!running && activity.value("reply", std::string{}).empty() && !path.empty()) {
                if (auto reply = cached_last_reply(path); !reply.empty()) activity["reply"] = std::move(reply);
            }
            if (!activity.empty()) card["activity"] = std::move(activity);
            activities.push_back(std::move(card));
        };
        for (const auto& session : active) {
            if (!session.parent_session_id.empty() || !listed.insert(session.id).second) continue;
            const auto scope = session.no_workspace ? std::string{} : session.workspace_hash;
            const auto cwd = session.no_workspace ? std::string{} : session.cwd;
            const auto record = attention_record_for_session(scope, cwd, session.id, session.busy);
            if (!session.busy && session_attention_state_for(record) != SessionAttentionState::Unread) continue;
            add_activity(session_info_to_json(session, nullptr), session.id);
        }
        // Recent sessions that are no longer loaded can still hold an unread result.
        for (const auto& entry : recent.entries) {
            if (entry.active || entry.meta.archived || !listed.insert(entry.meta.id).second) continue;
            auto item = session_meta_to_json(entry.meta, entry.workspace_hash);
            if (item.value("attention_state", std::string{}) != "unread") continue;
            item["last_user_message_at"] = entry.meta.last_user_message_at;
            item["last_turn_outcome"] = entry.meta.last_turn_outcome;
            add_activity(std::move(item), entry.meta.id);
        }
        std::string selected = req.url_params.get("session") ? req.url_params.get("session") : "";
        std::string workspace = req.url_params.get("workspace") ? req.url_params.get("workspace") : "";
        if (selected.empty() && !offices.empty()) {
            selected = offices.front().value("id", std::string{});
            workspace = offices.front().value("workspace_hash", std::string{});
        }
        json result{{"offices", offices}, {"selected", nullptr}, {"agents", json::array()},
                    {"activities", activities},
                    {"complete", recent.progress.complete}, {"connected", true}};
        if (selected.empty()) return respond(200, result);
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
        if (!ws) return respond(404, {{"error", "office session not found"}, {"offices", offices}, {"activities", activities}});
        auto meta = find_session_meta_for_workspace(*ws, selected);
        if (!meta || meta->archived) {
            return respond(404, {{"error", "office session unavailable"}, {"offices", offices}, {"activities", activities}});
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
