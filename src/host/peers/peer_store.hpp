#pragma once
#include "share/invite_link.hpp"
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace acecode::peers {
struct PeerRecord {
    std::string id;
    share::InviteLink invite;
    std::string address;
    std::string identity;
    std::string session_id;
    std::string status = "connected";
    bool allow_questions = false;
    bool include_tools = false;
};

class PeerStore {
public:
    explicit PeerStore(const std::string& data_dir);
    std::vector<PeerRecord> list() const;
    std::optional<PeerRecord> find(const std::string& id) const;
    PeerRecord save(PeerRecord peer);
    bool erase(const std::string& id);
    void set_status(const std::string& id, const std::string& status);
private:
    std::string path_;
};
nlohmann::json peer_public_json(const PeerRecord& peer);
std::string peer_base_url(const PeerRecord& peer);

struct PeerResponse {
    int status = 0;
    std::string body;
    std::string content_type;
};

PeerRecord connect_peer(const share::InviteLink& invite);
PeerResponse request_peer(const PeerRecord& peer, const std::string& path,
                          const std::optional<nlohmann::json>& body = std::nullopt);
PeerRecord reconnect_peer(PeerStore& store, const PeerRecord& peer);
PeerResponse request_saved_peer(PeerStore& store, const std::string& id, const std::string& path);
} // namespace acecode::peers
