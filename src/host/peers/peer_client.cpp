#include "peer_store.hpp"
#include "network/proxy_resolver.hpp"
#include <cpr/cpr.h>
#include <stdexcept>
#include <memory>
#include <algorithm>
#include <chrono>

namespace acecode::peers {
std::string peer_base_url(const PeerRecord& peer) {
    if (!share::valid_invite_address(peer.address) || peer.invite.port < 1 || peer.invite.port > 65535)
        throw std::invalid_argument("invalid_address");
    auto host = peer.address;
    const auto zone = host.find('%');
    if (zone != std::string::npos) host.replace(zone, 1, "%25");
    if (host.find(':') != std::string::npos) host = "[" + host + "]";
    return "http://" + host + ":" + std::to_string(peer.invite.port);
}

PeerResponse request_peer(const PeerRecord& peer, const std::string& path,
                          const std::optional<nlohmann::json>& body) {
    if (path.rfind("/api/share/v1/", 0) != 0) throw std::invalid_argument("invalid_peer_route");
    const auto url = peer_base_url(peer) + path;
    auto proxy = network::proxy_options_for(url);
    if (proxy.resolved.url.empty()) proxy.proxies = cpr::Proxies{{"http", ""}, {"https", ""}};
    cpr::Session session;
    session.SetUrl(cpr::Url{url});
    session.SetHeader(cpr::Header{{"X-ACECode-Share", peer.invite.credential}, {"Content-Type", "application/json"}});
    session.SetProxies(proxy.proxies);
    session.SetProxyAuth(proxy.auth);
    session.SetConnectTimeout(cpr::ConnectTimeout{3000});
    session.SetTimeout(cpr::Timeout{3000});
    session.SetRedirect(cpr::Redirect{false});
    // Shared only with this synchronous HTTP call's write callback.
    auto received = std::make_shared<std::string>();
    session.SetWriteCallback(cpr::WriteCallback{[received](std::string_view bytes, intptr_t) {
        if (received->size() + bytes.size() > 32u * 1024u * 1024u) return false;
        received->append(bytes); return true;
    }});
    if (body) session.SetBody(cpr::Body{body->dump()});
    const auto response = body ? session.Post() : session.Get();
    if (response.error.code != cpr::ErrorCode::OK) return {0, {}, {}};
    const auto mime = response.header.find("content-type");
    return {static_cast<int>(response.status_code), std::move(*received),
        mime == response.header.end() ? "application/json" : mime->second};
}

PeerRecord connect_peer(const share::InviteLink& invite) {
    PeerRecord peer;
    peer.invite = invite;
    for (const auto& address : invite.addresses) {
        peer.address = address;
        const auto response = request_peer(peer, "/api/share/v1/hello", nlohmann::json::object());
        if (response.status != 200) continue;
        try {
            const auto hello = nlohmann::json::parse(response.body);
            if (hello.at("version") != 1) throw std::runtime_error("unsupported_version");
            peer.identity = hello.at("identity"); peer.session_id = hello.at("session_id");
            peer.allow_questions = hello.at("allow_questions"); peer.include_tools = hello.at("include_tools");
            if (peer.identity.empty() || peer.session_id.empty()) continue;
            return peer;
        } catch (const nlohmann::json::exception&) { continue; }
    }
    throw std::runtime_error("connection_failed");
}

PeerRecord reconnect_peer(PeerStore& store, const PeerRecord& peer) {
    auto invite = peer.invite;
    invite.addresses.erase(std::remove(invite.addresses.begin(), invite.addresses.end(), peer.address), invite.addresses.end());
    invite.addresses.insert(invite.addresses.begin(), peer.address);
    auto connected = connect_peer(invite);
    if (connected.identity != peer.identity || connected.session_id != peer.session_id)
        throw std::runtime_error("peer_identity_changed");
    connected.id = peer.id;
    return store.save(std::move(connected));
}

PeerResponse request_saved_peer(PeerStore& store, const std::string& id, const std::string& path) {
    auto peer = store.find(id);
    if (!peer) return {404, "{\"error\":\"unknown_peer\"}", "application/json"};
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (peer->invite.expires_at <= now) {
        store.set_status(id, "expired");
        return {401, "{\"error\":\"expired\"}", "application/json"};
    }
    auto response = request_peer(*peer, path);
    if (response.status == 0) {
        try { response = request_peer(reconnect_peer(store, *peer), path); }
        catch (const std::exception&) { store.set_status(id, "disconnected"); }
    }
    if (response.status == 401) store.set_status(id, "invalid");
    return response;
}
} // namespace acecode::peers
