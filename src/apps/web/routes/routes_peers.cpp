#include "web/server_impl.hpp"
#include "share/invite_link.hpp"
#include "utils/url_encoding.hpp"

namespace acecode::web {
using nlohmann::json;
namespace {
crow::response peer_response(const crow::request& req, int status, std::string body = {}) {
    crow::response response(status, std::move(body));
    add_loopback_cors_headers(req, response);
    response.set_header("Content-Type", "application/json");
    response.set_header("Cache-Control", "no-store");
    return response;
}
}
void WebServer::Impl::register_peers() {
    CROW_ROUTE(app, "/api/peers/connect").methods(crow::HTTPMethod::POST)
    ([this](const crow::request& req) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        const auto body = json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("link") || !body["link"].is_string())
            return peer_response(req,400, json{{"error", "invalid_link"}}.dump());
        auto invite = share::decode_invite_link(body["link"].get<std::string>(), now_unix_ms() / 1000);
        if (!invite) return peer_response(req,400, json{{"error", invite.error}}.dump());
        try {
            auto peer = peers->save(peers::connect_peer(invite.value));
            return peer_response(req,200, peers::peer_public_json(peer).dump());
        } catch (const std::exception& error) {
            return peer_response(req,502, json{{"error", std::string(error.what()) == "unsupported_version"
                ? "unsupported_version" : "connection_failed"}}.dump());
        }
    });
    CROW_ROUTE(app, "/api/peers").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        auto result = json::array();
        for (auto peer : peers->list()) {
            if (peer.invite.expires_at <= now_unix_ms() / 1000) peer.status = "expired";
            result.push_back(peers::peer_public_json(peer));
        }
        return peer_response(req,200, json{{"peers", result}}.dump());
    });
    CROW_ROUTE(app, "/api/peers/<string>").methods(crow::HTTPMethod::DELETE)
    ([this](const crow::request& req, const std::string& id) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        if (!peers->erase(id)) return peer_response(req,404);
        std::vector<std::shared_ptr<ShareConnection>> connections;
        { std::lock_guard<std::mutex> lock(sharing_mu);
          for (const auto& entry : sharing_connections)
              if (entry.second->peer && entry.second->peer->id == id) connections.push_back(entry.second); }
        for (const auto& state : connections) state->disconnect("peer_deleted");
        return peer_response(req,200, "{\"ok\":true}");
    });
    CROW_ROUTE(app, "/api/peers/<string>/messages").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req, const std::string& id) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        auto peer = peers->find(id);
        if (!peer) return peer_response(req,404);
        std::string query;
        try {
            if (const auto value = req.url_params.get("before")) {
                if (!decode_history_cursor(value)) return peer_response(req, 400);
                query = "?before=" + utils::percent_encode_query_component(value);
            }
        } catch (const std::exception&) { return peer_response(req,400); }
        auto response = peers::request_saved_peer(*peers, id, "/api/share/v1/messages" + query);
        if (response.status == 401) peers->set_status(id, "invalid");
        crow::response out(response.status ? response.status : 502, response.body);
        out.set_header("Content-Type", "application/json"); out.set_header("Cache-Control", "no-store");
        return with_cors(req, std::move(out));
    });
    CROW_ROUTE(app, "/api/peers/<string>/attachments/<string>/blob").methods(crow::HTTPMethod::GET)
    ([this](const crow::request& req, const std::string& peer_id, const std::string& attachment_id) {
        if (auto denied = require_auth(req)) return std::move(*denied);
        const auto peer = peers->find(peer_id);
        if (!peer || !is_valid_attachment_id(attachment_id)) return peer_response(req,404);
        const auto response = peers::request_saved_peer(*peers, peer_id, "/api/share/v1/attachments/" + attachment_id + "/blob");
        if (response.status != 200 || (response.content_type != "image/png" && response.content_type != "image/jpeg" &&
            response.content_type != "image/gif" && response.content_type != "image/webp" && response.content_type != "image/bmp"))
            return peer_response(req,404);
        crow::response out(200, response.body);
        out.set_header("Content-Type", response.content_type);
        out.set_header("X-Content-Type-Options", "nosniff"); out.set_header("Cache-Control", "no-store");
        return with_cors(req, std::move(out));
    });
}
} // namespace acecode::web
