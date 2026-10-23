#include "server_impl.hpp"

#include "session/session_load_metrics.hpp"
#include "session/session_pin_store.hpp"
#include "channels/core/host.hpp"
#include "web/handlers/channels_handler.hpp"

#include <algorithm>
#include <set>

namespace acecode::web {
using nlohmann::json;

json WebServer::Impl::sidebar_session_page(const acecode::desktop::WorkspaceMeta& ws,
                                           bool no_workspace, int limit,
                                           std::set<std::string> expanded,
                                           const std::string& reveal,
                                           const json& move) const {
    SessionLoadTimer timer("sidebar_tree", no_workspace ? "no-workspace" : ws.hash);
    std::lock_guard<std::mutex> lock(sidebar_sessions_mu);
    std::vector<std::string> directories;
    if (no_workspace) {
        for (const auto& cwd : list_no_workspace_session_cwds(no_workspace_cache_root())) {
            directories.push_back(SessionStorage::get_project_dir(cwd));
        }
    } else {
        directories.push_back(SessionStorage::get_project_dir(ws.cwd));
    }
    const auto metas = sidebar_metadata_cache.read(directories);
    std::map<std::string, const SessionMeta*> by_id;
    json summaries = json::array();
    const auto pin_path = no_workspace ? no_workspace_pinned_sessions_path() : pinned_sessions_path_for_cwd(ws.cwd);
    const auto pins = session_pins::read_pinned_sessions_state(pin_path).session_ids;
    const std::set<std::string> pinned(pins.begin(), pins.end());
    std::map<std::string, json> active_bodies;
    std::map<std::string, std::string> active_storage_cwds;
    std::map<std::string, SessionMeta> active_metadata;
    const auto acceptable = [&](const SessionMeta& meta) {
        return !meta.archived && meta.parent_session_id.empty() && meta.no_workspace == no_workspace;
    };
    for (const auto& meta : metas) if (acceptable(meta)) by_id[meta.id] = &meta;
    if (deps.session_client) {
        for (const auto& session : deps.session_client->list_sessions()) {
            if (session.no_workspace != no_workspace || !session.parent_session_id.empty()) continue;
            if (!no_workspace && session.workspace_hash != ws.hash) continue;
            auto found = by_id.find(session.id);
            if (found == by_id.end()) {
                const auto existing = std::find_if(metas.begin(), metas.end(), [&](const auto& meta) { return meta.id == session.id; });
                if (existing != metas.end() && !acceptable(*existing)) continue;
                // An active worktree session can persist under its own cwd rather
                // than the registered workspace path. Preserve its real metadata.
                auto meta = no_workspace ? find_no_workspace_session_meta(session.id)
                    : std::optional<SessionMeta>{SessionStorage::read_meta(SessionStorage::meta_path(
                        SessionStorage::get_project_dir(session.cwd.empty() ? ws.cwd : session.cwd), session.id))};
                if (meta && !meta->id.empty()) {
                    if (!acceptable(*meta)) continue;
                    active_metadata[session.id] = std::move(*meta);
                    by_id[session.id] = &active_metadata.at(session.id);
                    found = by_id.find(session.id);
                }
            }
            active_bodies[session.id] = session_info_to_json(session, found == by_id.end() ? nullptr : found->second);
            active_storage_cwds[session.id] = session.cwd;
        }
    }
    std::set<std::string> ids;
    for (const auto& [id, meta] : by_id) ids.insert(id);
    for (const auto& [id, body] : active_bodies) ids.insert(id);
    for (const auto& id : ids) {
        if (pinned.count(id)) continue;
        json summary;
        if (auto active = active_bodies.find(id); active != active_bodies.end()) {
            summary = {{"id", id}, {"created_at", active->second.value("created_at", "")},
                       {"updated_at", active->second.value("updated_at", "")},
                       {"loop_execution", active->second.value("loop_execution", json::object())}};
        } else {
            const auto& meta = *by_id.at(id);
            summary = {{"id", id}, {"created_at", meta.created_at}, {"updated_at", meta.updated_at},
                       {"loop_execution", {{"loop_id", meta.loop_id}}}};
        }
        summaries.push_back(std::move(summary));
    }
    const auto layout_path = pin_path.parent_path() / "sidebar_sessions.json";
    auto layout = read_sidebar_layout(layout_path);
    auto tree = build_sidebar_tree(summaries, layout);
    if (move.is_object() && !move.empty()) {
        std::string error;
        std::vector<std::string> sources;
        if (move.contains("sources")) {
            if (!move["sources"].is_array()) return {{"error", "SIDEBAR_INVALID_SOURCES"}};
            for (const auto& source : move["sources"]) {
                if (!source.is_string()) return {{"error", "SIDEBAR_INVALID_SOURCES"}};
                sources.push_back(source.get<std::string>());
            }
        } else if (move.contains("source") && move["source"].is_string()) {
            sources.push_back(move["source"].get<std::string>());
        }
        if (!move.contains("target") || !move["target"].is_string()
            || !move.contains("placement") || !move["placement"].is_string()) {
            return {{"error", "SIDEBAR_INVALID_PLACEMENT"}};
        }
        if (!move_sidebar_nodes(tree, layout, sources, move["target"].get<std::string>(),
                                move["placement"].get<std::string>(), error)) return {{"error", error}};
        if (!write_sidebar_layout(layout_path, layout, error)) return {{"error", "SIDEBAR_SAVE_FAILED"}, {"detail", error}};
        tree = build_sidebar_tree(summaries, layout);
        for (const auto& source : sources) {
            const auto& moved = tree.nodes.at(source);
            const auto root = moved.parent.empty() ? moved.key : moved.parent;
            if (!moved.parent.empty()) expanded.insert(moved.parent);
            const auto index = std::find(tree.roots.begin(), tree.roots.end(), root) - tree.roots.begin();
            if (limit > 0) limit = std::max(limit, static_cast<int>((index / 5 + 1) * 5));
        }
    }
    for (const auto& [key, node] : tree.nodes) {
        if (reveal.empty() || node.session_id != reveal) continue;
        const auto root = node.parent.empty() ? key : node.parent;
        if (!node.parent.empty()) expanded.insert(node.parent);
        const auto index = std::find(tree.roots.begin(), tree.roots.end(), root) - tree.roots.begin();
        if (limit > 0) limit = std::max(limit, static_cast<int>((index / 5 + 1) * 5));
    }
    std::string remote_control_id;
    if (deps.app_config) {
        std::shared_lock<std::shared_mutex> config_lock(app_config_mu);
        remote_control_id = deps.app_config->remote_control.bound_session_id;
    }
    const auto body_for = [&](const std::string& id) {
        json body = active_bodies.count(id) ? active_bodies.at(id)
            : session_meta_to_json(*by_id.at(id), no_workspace ? std::string{} : ws.hash);
        body["remote_control_bound"] = !remote_control_id.empty() && remote_control_id == id;
        return body;
    };
    std::map<std::string, std::string> loop_names;
    if (deps.loop_store) {
        for (const auto& task : deps.loop_store->list_loops()) loop_names[task.id] = task.name;
    }
    json sessions = json::array();
    const auto emit = [&](const SidebarNode& node, std::size_t rank) {
        auto body = body_for(node.session_id);
        if (node.group) {
            json members = json::array();
            bool unread = false;
            for (const auto& key : node.children) {
                // Collapsed groups need only attention summaries, not the full
                // session/expert/worktree serialization of every hidden row.
                const auto& id = tree.nodes.at(key).session_id;
                const auto active = active_bodies.find(id);
                const auto scope = no_workspace ? std::string{} : ws.hash;
                const auto& cwd = active != active_bodies.end()
                    ? active_storage_cwds.at(id) : by_id.at(id)->cwd;
                const bool busy = active != active_bodies.end() && active->second.value("busy", false);
                const auto record = attention_record_for_session(scope, cwd, id, busy);
                auto status = attention_payload_for_record(id, scope, cwd, record);
                status["id"] = id;
                status["status_cursor"] = status["cursor"];
                status["no_workspace"] = no_workspace;
                unread = unread || status.value("state", std::string{}) == "unread";
                members.push_back(std::move(status));
            }
            body = {{"id", "sidebar-group:" + node.key}, {"title", loop_names[node.loop_id]},
                {"sidebar_group", true}, {"sidebar_group_name", loop_names[node.loop_id]},
                {"sidebar_members", std::move(members)}, {"sidebar_latest_session_id", node.session_id},
                {"created_at", body.value("created_at", std::string{})}, {"updated_at", body.value("updated_at", std::string{})},
                {"cwd", body.value("cwd", std::string{})}, {"workspace_hash", no_workspace ? std::string{} : ws.hash},
                {"no_workspace", no_workspace}, {"active", false}, {"busy", false}, {"status", "idle"},
                {"attention_state", unread ? "unread" : "read"}, {"read_state", unread ? "unread" : "read"},
                {"loop_execution", {{"loop_id", node.loop_id}}}};
        }
        body["sidebar_key"] = node.key;
        body["sidebar_parent_key"] = node.parent;
        body["sidebar_child_count"] = node.children.size();
        body["sidebar_rank"] = rank;
        body["sidebar_expanded"] = expanded.count(node.key) != 0;
        sessions.push_back(std::move(body));
    };
    const auto count = limit > 0 ? std::min(tree.roots.size(), static_cast<std::size_t>(limit)) : tree.roots.size();
    for (std::size_t rank = 0; rank < count; ++rank) {
        const auto& node = tree.nodes.at(tree.roots[rank]);
        emit(node, rank);
        if (expanded.count(node.key)) {
            for (std::size_t child = 0; child < node.children.size(); ++child) emit(tree.nodes.at(node.children[child]), child);
        }
    }
    for (const auto& id : pins) if (ids.count(id)) sessions.push_back(body_for(id));
    if (deps.channel_host) annotate_channel_bound(sessions, deps.channel_host->bound_sessions());
    return {{"sessions", std::move(sessions)}, {"total", tree.roots.size()}, {"total_exact", true},
            {"has_more", count < tree.roots.size()}, {"sidebar_tree", true}, {"visible_roots", count}};
}

crow::response WebServer::Impl::handle_sidebar_sessions(const crow::request& req,
                                                       const std::string& workspace_hash) {
    if (req.method == crow::HTTPMethod::Options) return cors_preflight(req);
    if (auto rejected = require_auth(req)) return std::move(*rejected);
    const auto respond = [&](int status, const json& body) {
        crow::response response(status, body.dump());
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        return with_cors(req, std::move(response));
    };
    const bool no_workspace = workspace_hash.empty();
    acecode::desktop::WorkspaceMeta ws;
    if (!no_workspace) {
        auto resolved = resolve_workspace(workspace_hash);
        if (!resolved) return respond(404, {{"error", "workspace not found"}});
        ws = *resolved;
    }
    try {
        std::set<std::string> expanded;
        if (const auto* raw = req.url_params.get("expanded")) {
            auto parsed = json::parse(raw);
            if (!parsed.is_array()) return respond(400, {{"error", "expanded array required"}});
            for (const auto& key : parsed) if (key.is_string()) expanded.insert(key.get<std::string>());
        }
        const auto* raw_reveal = req.url_params.get("reveal");
        const auto move = req.method == crow::HTTPMethod::POST ? json::parse(req.body) : json{};
        if (!move.is_null() && (!move.is_object() || (!move.contains("source") && !move.contains("sources"))
            || !move.contains("target") || !move.contains("placement"))) {
            return respond(400, {{"error", "source or sources, target and placement required"}});
        }
        auto body = sidebar_session_page(ws, no_workspace, parse_session_list_limit(req.url_params.get("limit")),
                                         std::move(expanded), raw_reveal ? raw_reveal : "", move);
        const auto error = body.value("error", std::string{});
        return respond(error.empty() ? 200 : error == "SIDEBAR_SAVE_FAILED" ? 500 : 409, body);
    } catch (const json::exception&) {
        return respond(400, {{"error", "invalid sidebar request"}});
    }
}

} // namespace acecode::web
