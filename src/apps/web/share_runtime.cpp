#include "server_impl.hpp"
#include "share/invite_link.hpp"
#include "share/share_projection.hpp"
#include "utils/state_file.hpp"
#include "utils/uuid.hpp"

namespace acecode::web {
using nlohmann::json;

void WebServer::Impl::initialize_sharing() {
    const auto directory = path_to_utf8(path_from_utf8(projects_dir()).parent_path());
    shares = std::make_shared<share::ShareStore>(directory);
    peers = std::make_shared<peers::PeerStore>(directory);
    const auto ref = sharing_lifetime.ref(*this);
    sharing_expiry = JoiningThread([ref](StopToken stop) {
        int ticks = 0;
        while (!stop.wait_for(std::chrono::seconds(1))) {
            ref.with([&](Impl& owner) {
                try {
                    owner.expire_share_connections();
                    if (++ticks % 60 == 0) owner.reconcile_share_listener();
                } catch (const std::exception&) { LOG_WARN("[share] maintenance failed"); }
            });
        }
    });
    reconcile_share_listener();
}

void WebServer::Impl::stop_sharing() {
    std::lock_guard<std::mutex> stop_lock(sharing_stop_mu);
    if (sharing_stopped.exchange(true)) return;
    sharing_expiry.request_stop(); sharing_expiry.join();
    std::vector<std::shared_ptr<ShareConnection>> connections;
    {
        std::lock_guard<std::mutex> lock(sharing_mu);
        for (const auto& entry : sharing_connections) connections.push_back(entry.second);
        pending_sharing.clear();
    }
    for (const auto& state : connections) state->disconnect("server_stopping");
    sharing_workers.shutdown();
    sharing_lifetime.revoke();
    for (const auto& state : connections) {
        ScopedSubscription subscription;
        { std::lock_guard<std::mutex> lock(state->mu);
          subscription = std::move(state->subscription); state->connection = nullptr; }
    }
    { std::lock_guard<std::mutex> lock(sharing_mu); sharing_connections.clear(); }
}

void WebServer::Impl::reconcile_share_listener() {
    if (!deps.remote_web_proxy || !shares) return;
    std::lock_guard<std::mutex> proxy_lock(remote_web_proxy_mu);
    bool enabled = false;
    int configured_port = 0;
    {
        std::shared_lock<std::shared_mutex> lock(app_config_mu);
        const auto* cfg = deps.app_config ? &deps.app_config->web : deps.web_cfg;
        if (cfg) { enabled = cfg->remote_enabled; configured_port = cfg->remote_port; }
    }
    const auto current = deps.remote_web_proxy->status();
    if (!deps.dangerous && (enabled || shares->has_active(now_unix_ms() / 1000))) {
        if (!current.running || (configured_port != 0 && current.port != configured_port))
            deps.remote_web_proxy->start(configured_port, runtime_port);
    } else if (current.running || current.starting) deps.remote_web_proxy->stop();
}

void WebServer::Impl::expire_share_connections() {
    const auto now = now_unix_ms() / 1000;
    std::vector<std::shared_ptr<ShareConnection>> connections;
    {
        std::lock_guard<std::mutex> lock(sharing_mu);
        for (const auto& entry : sharing_connections) connections.push_back(entry.second);
        for (auto it = pending_sharing.begin(); it != pending_sharing.end();) {
            if (it->second->accepted_at + 30 < now) it = pending_sharing.erase(it);
            else ++it;
        }
    }
    for (const auto& state : connections) {
        if (state->share) {
            const auto auth = shares->authorize(state->share->credential(), now);
            if (!auth.share) state->disconnect(auth.error);
        }
    }
    sharing_workers.reap();
}

std::string WebServer::Impl::share_link(const share::ShareRecord& record) {
    if (!deps.remote_web_proxy) throw std::runtime_error("share_listener_unavailable");
    const auto proxy = deps.remote_web_proxy->status();
    if (!proxy.running) throw std::runtime_error("share_listener_unavailable");
    auto addresses = rank_remote_web_hosts(deps.remote_web_hosts ? deps.remote_web_hosts() : discover_remote_web_hosts(),
        {}, proxy.ipv6 ? "" : "0.0.0.0", deps.remote_web_computer_name ? deps.remote_web_computer_name() : discover_remote_web_computer_name());
    if (addresses.empty()) throw std::runtime_error("no_lan_address");
    share::InviteLink invite{record.owner, record.title, addresses, proxy.port, record.credential(), record.expires_at};
    for (;;) {
        try { return share::encode_invite_link(invite); }
        catch (const std::invalid_argument& error) {
            if (std::string(error.what()) != "too_long" || invite.addresses.size() <= 1) throw;
            invite.addresses.pop_back();
        }
    }
}

std::optional<crow::response> WebServer::Impl::require_share_auth(const crow::request& req, share::ShareRecord& record) {
    auto auth = shares->authorize(req.get_header_value("X-ACECode-Share"), now_unix_ms() / 1000);
    if (deps.dangerous || !req.get_header_value("Origin").empty()) auth = {{}, "guest_origin_forbidden"};
    if (auth.share) {
        if (auto id = req.url_params.get("session_id"); id && auth.share->session_id != id)
            auth = {{}, "wrong_session"};
    }
    LOG_INFO("[share] id=" + (auth.share ? auth.share->id : std::string("unknown")) +
        " source=" + req.remote_ip_address + " result=" + (auth.share ? "allowed" : auth.error));
    if (!auth.share) return crow::response(401, json{{"error", auth.error}}.dump());
    record = std::move(*auth.share);
    return {};
}

std::shared_ptr<SessionEntry> WebServer::Impl::acquire_shared_session(const share::ShareRecord& record) {
    if (!deps.session_registry) return {};
    auto entry = deps.session_registry->acquire(record.session_id);
    if (!entry) {
        SessionOptions options; options.cwd = record.cwd; options.inherit_dangerous_mode = false;
        if (!deps.session_registry->resume(record.session_id, options)) return {};
        entry = deps.session_registry->acquire(record.session_id);
    }
    return entry;
}

json WebServer::Impl::shared_messages(const share::ShareRecord& record, const SessionHistoryRequest& request) {
    const auto entry = acquire_shared_session(record);
    if (!entry || !entry->sm) throw std::runtime_error("session_unavailable");
    const auto path = SessionStorage::session_path(SessionStorage::get_project_dir(entry->cwd), record.session_id);
    std::error_code ec;
    if (!std::filesystem::exists(path_from_utf8(path), ec)) return {{"messages", json::array()}, {"before", ""}, {"has_older", false}};
    auto bounded = request; bounded.limit = std::clamp<std::size_t>(request.limit, 1, 200);
    const auto page = load_session_history_page(path, bounded);
    auto messages = json::array();
    for (const auto& item : page.messages) {
        auto projected = share::project_message(chat_message_to_json(item.message), record.include_tools);
        if (!projected.is_null()) messages.push_back(std::move(projected));
    }
    return {{"messages", messages}, {"before", page.before}, {"has_older", page.has_more}};
}
} // namespace acecode::web
