#include "web/server_impl.hpp"
#include "share/share_projection.hpp"

namespace acecode::web {
using nlohmann::json;
void WebServer::Impl::register_peer_socket() {
    CROW_WEBSOCKET_ROUTE(app, "/ws/peers/<string>")
    .onaccept([this](const crow::request& req, void** userdata) {
        if (shutdown_requested.load()) return false;
        std::string token;
        if (const auto value = req.url_params.get("token")) token = value;
        if (auth_result_for_request(req, req.get_header_value("X-ACECode-Token"), token) != AuthResult::Allowed) return false;
        auto peer = peers->find(req.url.substr(std::string("/ws/peers/").size()));
        if (!peer || peer->invite.expires_at <= now_unix_ms() / 1000) return false;
        auto state = std::make_shared<ShareConnection>();
        state->peer = std::move(peer); state->accepted_at = now_unix_ms() / 1000;
        state->upstream = std::make_shared<network::WebSocketClient>();
        std::lock_guard<std::mutex> lock(sharing_mu);
        *userdata = state.get(); pending_sharing.emplace(state.get(), std::move(state));
        return true;
    })
    .onopen([this](crow::websocket::connection& connection) {
        open_sharing_connection(connection);
        std::shared_ptr<ShareConnection> state;
        { std::lock_guard<std::mutex> lock(sharing_mu);
          auto found = sharing_connections.find(&connection);
          if (found != sharing_connections.end()) state = found->second; }
        if (!state || !state->peer) return;
        const std::weak_ptr<ShareConnection> weak = state;
        const auto peer = *state->peer;
        const auto upstream = state->upstream;
        if (!sharing_workers.spawn([weak, peer, upstream, store = peers](StopToken stop) {
            network::WebSocketConnectOptions options;
            options.url = "ws" + peers::peer_base_url(peer).substr(4) + "/ws/share/v1";
            options.connect_timeout = std::chrono::seconds(3);
            options.headers.emplace_back("X-ACECode-Share", peer.invite.credential);
            std::string error;
            if (!upstream->connect(options, &error)) {
                bool reconnected = false;
                if (!stop.stop_requested()) {
                    try {
                        const auto latest = peers::reconnect_peer(*store, peer);
                        options.url = "ws" + peers::peer_base_url(latest).substr(4) + "/ws/share/v1";
                        reconnected = upstream->connect(options, &error);
                    } catch (const std::exception&) {}
                }
                if (!reconnected) { if (auto target = weak.lock()) target->disconnect("connection_failed"); return; }
            }
            upstream->send_text("{\"type\":\"subscribe\"}", std::chrono::seconds(2), &error);
            while (!stop.stop_requested()) {
                network::WebSocketMessage message;
                const auto received = upstream->receive(message, std::chrono::milliseconds(100), &error);
                if (received == network::WebSocketRecv::Timeout) continue;
                if (received != network::WebSocketRecv::Message) break;
                if (message.binary) break;
                auto body = json::parse(message.data, nullptr, false);
                if (!body.is_object() || !body.contains("type") || !body["type"].is_string()) break;
                const auto type = body["type"].get<std::string>();
                if (type != "hello" && type != "changed" && type != "token" && type != "progress" &&
                    type != "ask_delta" && type != "ask_done" && type != "error" && type != "pong") continue;
                if (auto target = weak.lock()) target->send(share::redact_share_json(std::move(body)));
                else break;
            }
            if (!stop.stop_requested()) {
                try {
                    const auto reason = upstream->close_reason();
                    store->set_status(peer.id, reason == "revoked" || reason == "expired" || reason == "share_invalid" ? "invalid" : "disconnected");
                } catch (const std::exception&) {}
            }
            if (auto target = weak.lock()) target->disconnect("remote_disconnected");
            upstream->abort();
        })) state->disconnect("server_stopping");
    })
    .onmessage([this](crow::websocket::connection& connection, const std::string& data, bool binary) {
        if (binary || data.size() > 65536) { connection.close("invalid_message"); return; }
        auto body = json::parse(data, nullptr, false);
        if (!body.is_object() || !body.contains("type") || !body["type"].is_string()) return;
        const auto type = body["type"].get<std::string>();
        if (type != "subscribe" && type != "unsubscribe" && type != "ping" && type != "ask_start" && type != "ask_stop") return;
        std::shared_ptr<ShareConnection> state;
        { std::lock_guard<std::mutex> lock(sharing_mu);
          auto found = sharing_connections.find(&connection);
          if (found != sharing_connections.end()) state = found->second; }
        if (!state || !state->peer) return;
        if (type == "ask_start" && !state->peer->allow_questions) {
            state->send({{"type", "error"}, {"error", "read_only"}}); return;
        }
        if (type == "ask_start") body["name"] = discover_remote_web_computer_name().value_or("Guest");
        std::string error;
        if (!state->upstream->send_text(body.dump(), std::chrono::seconds(2), &error))
            state->send({{"type", "error"}, {"error", "connection_failed"}});
    })
    .onclose([this](crow::websocket::connection& connection, const std::string&, std::uint16_t) {
        close_sharing_connection(connection);
    });
}
} // namespace acecode::web
