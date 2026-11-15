#include "peer_store.hpp"
#include "utils/state_file.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"
#include <stdexcept>

namespace acecode::peers {
using nlohmann::json;
namespace {
json serialize(const PeerRecord& p) {
    return {{"id", p.id}, {"link", share::encode_invite_link(p.invite)},
        {"address", p.address}, {"identity", p.identity}, {"session_id", p.session_id},
        {"status", p.status}, {"allow_questions", p.allow_questions}, {"include_tools", p.include_tools}};
}
std::optional<PeerRecord> deserialize(const json& value) {
    try {
        auto invite = share::decode_invite_link(value.at("link").get<std::string>(), 0);
        if (!invite) return {};
        PeerRecord p;
        p.id = value.at("id"); p.invite = std::move(invite.value);
        p.address = value.at("address"); p.identity = value.at("identity");
        p.session_id = value.at("session_id"); p.status = value.at("status");
        p.allow_questions = value.at("allow_questions"); p.include_tools = value.at("include_tools");
        if (!share::valid_invite_address(p.address)) return {};
        return p;
    } catch (const std::exception&) { return {}; }
}
} // namespace

PeerStore::PeerStore(const std::string& data_dir)
    : path_(path_to_utf8(path_from_utf8(data_dir) / "peers.json")) {}

std::vector<PeerRecord> PeerStore::list() const {
    std::vector<PeerRecord> result;
    const auto state = read_state_json_at(path_);
    for (const auto& entry : state.items()) {
        auto p = deserialize(entry.value());
        if (p && p->id == entry.key()) result.push_back(std::move(*p));
    }
    return result;
}

std::optional<PeerRecord> PeerStore::find(const std::string& id) const {
    const auto state = read_state_json_at(path_);
    if (!state.contains(id)) return {};
    auto peer = deserialize(state.at(id));
    return peer && peer->id == id ? peer : std::nullopt;
}

PeerRecord PeerStore::save(PeerRecord peer) {
    if (peer.id.empty()) peer.id = generate_uuid_v7();
    const auto result = update_state_json_at(path_, [peer](json& state) mutable {
        for (const auto& entry : state.items()) {
            auto existing = deserialize(entry.value());
            if (existing && existing->identity == peer.identity && existing->session_id == peer.session_id) {
                peer.id = existing->id; break;
            }
        }
        state[peer.id] = serialize(peer); return true;
    });
    if (!result) throw std::runtime_error("peer_save_failed");
    for (const auto& entry : result.state.items()) {
        auto saved = deserialize(entry.value());
        if (saved && saved->identity == peer.identity && saved->session_id == peer.session_id) return *saved;
    }
    throw std::runtime_error("peer_save_failed");
}

bool PeerStore::erase(const std::string& id) {
    const auto result = update_state_json_at(path_, [id](json& state) { return state.erase(id) > 0; });
    if (!result) throw std::runtime_error("peer_save_failed");
    return result.changed;
}

void PeerStore::set_status(const std::string& id, const std::string& status) {
    if (!update_state_json_at(path_, [id, status](json& state) {
        if (!state.contains(id)) return false;
        state[id]["status"] = status; return true;
    })) throw std::runtime_error("peer_save_failed");
}

json peer_public_json(const PeerRecord& p) {
    return {{"id", p.id}, {"owner", p.invite.owner}, {"title", p.invite.title},
        {"address", p.address}, {"status", p.status}, {"expires_at", p.invite.expires_at},
        {"credential_suffix", p.invite.credential.substr(p.invite.credential.size() - 4)},
        {"allow_questions", p.allow_questions}, {"include_tools", p.include_tools},
        {"mode", p.allow_questions ? "interactive" : "read_only"}};
}
} // namespace acecode::peers
