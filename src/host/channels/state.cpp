#include "state.hpp"
#include "owner_lock.hpp"
#include "utils/atomic_file.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace acecode::channels {
namespace {
bool identifier(const std::string& value, const std::string& suffix, bool dash) {
    if (value.size() <= suffix.size() || value.size() > 100 ||
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    const auto end = value.end() - static_cast<std::ptrdiff_t>(suffix.size());
    return std::all_of(value.begin(), end, [dash](char c) {
        return (c >= '0' && c <= '9') || (dash && c == '-');
    });
}
Json empty_state() {
    return {{"version", 1}, {"enabled", false}, {"access", Json::object()}, {"profile", ""},
            {"bindings", Json::object()}, {"receipts", Json::array()}};
}
std::string receipt_key(const Address& address, const std::string& id) {
    if (id.empty() || id.size() > 256) throw std::runtime_error("Invalid message id");
    return Json::array({address.account, address.chat, address.sender, id}).dump();
}
void validate_configuration(const Json& data) {
    if (!data.is_object() || data.at("version") != 1 || !data.at("enabled").is_boolean() ||
        !data.at("access").is_object()) throw std::runtime_error("Invalid channel configuration");
    const auto profile = data.value("profile", std::string{});
    if (!profile.empty() && (profile.size() != 32 || !std::all_of(profile.begin(), profile.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))) throw std::runtime_error("Invalid channel credential profile");
    for (const auto& item : data.at("access").items()) {
        if (!valid_peer(item.key()) || !item.value().is_array() || item.value().size() > 1024)
            throw std::runtime_error("Invalid channel access list");
        for (const auto& jid : item.value()) {
            if (!jid.is_string() || (!valid_peer(jid.get<std::string>()) &&
                !valid_group(jid.get<std::string>()))) throw std::runtime_error("Invalid channel peer");
        }
    }
}
void validate(const Json& data) {
    validate_configuration(data);
    if (!data.at("bindings").is_object() || !data.at("receipts").is_array() ||
        data.at("receipts").size() > 4096 || data.at("bindings").size() > 1024)
        throw std::runtime_error("Invalid channel state");
    for (const auto& item : data.at("bindings").items()) {
        const auto a = Address::parse(item.value().at("address"));
        const auto id = item.value().at("session_id").get<std::string>();
        if (!a.valid() || item.key() != a.key() || id.empty() || id.size() > 128 ||
            !std::all_of(id.begin(), id.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_';
            })) throw std::runtime_error("Invalid channel binding");
    }
    for (const auto& receipt : data.at("receipts")) {
        if (!receipt.is_string() || receipt.get_ref<const std::string&>().size() > 1024)
            throw std::runtime_error("Invalid channel receipt");
    }
}
Json read_json(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 4 * 1024 * 1024)
        throw std::runtime_error("Channel data is too large");
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read channel data");
    return Json::parse(in);
}
Json read_state(const std::filesystem::path& directory) {
    const auto path = directory / "state.json";
    auto data = std::filesystem::exists(path) ? read_json(path) : empty_state();
    validate(data);
    if (!data.contains("profile")) data["profile"] = "";
    return data;
}
Json configuration(const Json& data) {
    return {{"version", 1}, {"enabled", data.at("enabled")}, {"access", data.at("access")},
            {"profile", data.value("profile", std::string{})}};
}
Json read_configuration(const std::filesystem::path& directory) {
    const auto path = directory / "config.json";
    auto data = std::filesystem::exists(path) ? read_json(path) : configuration(read_state(directory));
    validate_configuration(data);
    return configuration(data);
}
} // namespace

bool valid_peer(const std::string& jid) {
    return identifier(jid, "@s.whatsapp.net", false) || identifier(jid, "@lid", false);
}
bool valid_group(const std::string& jid) { return identifier(jid, "@g.us", true); }
bool Address::valid() const {
    return valid_peer(account) && valid_peer(sender) &&
        (group ? valid_group(chat) : valid_peer(chat) && chat == sender);
}
std::string Address::key() const {
    return Json::array({"whatsapp", account, chat, group ? sender : std::string{}}).dump();
}
Json Address::json() const {
    return {{"account", account}, {"chat", chat}, {"sender", sender}, {"group", group}};
}
Address Address::parse(const Json& value) {
    return {value.at("account").get<std::string>(), value.at("chat").get<std::string>(),
            value.at("sender").get<std::string>(), value.value("group", false)};
}
bool path_inside(const std::filesystem::path& path, const std::filesystem::path& root) {
    std::error_code ec;
    const auto resolved = std::filesystem::canonical(path, ec);
    if (ec) return false;
    const auto base = std::filesystem::canonical(root, ec);
    if (ec || resolved == base) return false;
    auto p = resolved.begin();
    for (auto r = base.begin(); r != base.end(); ++r, ++p) {
        if (p == resolved.end()) return false;
#ifdef _WIN32
        if (_wcsicmp(p->c_str(), r->c_str()) != 0) return false;
#else
        if (*p != *r) return false;
#endif
    }
    return true;
}

State::State(std::filesystem::path directory)
    : directory_(std::move(directory)), data_(empty_state()) {}
void State::load() {
    std::lock_guard<std::mutex> lock(mu_);
    auto next = read_state(directory_);
    next.update(read_configuration(directory_));
    data_ = std::move(next);
}
void State::reload_history() {
    std::lock_guard<std::mutex> lock(mu_);
    auto history = read_state(directory_);
    data_["bindings"] = std::move(history.at("bindings"));
    data_["receipts"] = std::move(history.at("receipts"));
}
Json State::snapshot() const { std::lock_guard<std::mutex> lock(mu_); return data_; }
bool State::enabled() const { std::lock_guard<std::mutex> lock(mu_); return data_.at("enabled"); }
void State::commit(Json next) {
    validate(next);
    std::filesystem::create_directories(directory_);
    if (!atomic_write_file(path_to_utf8(directory_ / "state.json"), next.dump(2) + "\n", true))
        throw std::runtime_error("Cannot persist channel state");
    data_ = std::move(next);
}
void State::configure(const std::function<void(Json&)>& edit) {
    OwnerLock lock;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!lock.acquire(directory_ / "config.lock")) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Cannot save channel configuration. Retry saving.");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Merge explicit edits only. History writes and old hosts never write config.json.
    auto saved = read_configuration(directory_);
    auto next = data_;
    edit(saved); edit(next);
    validate_configuration(saved); validate(next);
    if (!atomic_write_file(path_to_utf8(directory_ / "config.json"), saved.dump(2) + "\n", true))
        throw std::runtime_error("Cannot persist channel configuration");
    data_ = std::move(next);
}
void State::set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mu_);
    configure([&](Json& next) { next["enabled"] = enabled; });
}
bool State::allowed(const Address& a, bool mentioned) const {
    if (!a.valid()) return false;
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = data_.at("access").find(a.account);
    if (it == data_.at("access").end()) return false;
    const bool peer = std::find(it->begin(), it->end(), a.sender) != it->end();
    return peer && (!a.group || (mentioned &&
        std::find(it->begin(), it->end(), a.chat) != it->end()));
}
void State::set_access(const std::string& account, const std::string& jid, bool allow) {
    if (!valid_peer(account) || (!valid_peer(jid) && !valid_group(jid)))
        throw std::runtime_error("Expected a WhatsApp account and peer/group JID");
    std::lock_guard<std::mutex> lock(mu_);
    configure([&](Json& next) {
        auto& list = next["access"][account];
        if (list.is_null()) list = Json::array();
        list.erase(std::remove(list.begin(), list.end(), Json(jid)), list.end());
        if (allow) list.push_back(jid);
    });
}
std::optional<std::string> State::session(const Address& a) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = data_.at("bindings").find(a.key());
    if (it == data_.at("bindings").end()) return std::nullopt;
    return it->at("session_id").get<std::string>();
}
void State::enable_with_access(const std::string& account, const std::vector<std::string>& peers,
                               const std::optional<std::string>& profile) {
    if (!valid_peer(account) || peers.empty() || peers.size() > 128 ||
        !std::all_of(peers.begin(), peers.end(), valid_peer))
        throw std::runtime_error("Invalid setup access list");
    std::lock_guard<std::mutex> lock(mu_);
    configure([&](Json& next) {
        auto& list = next["access"][account];
        if (list.is_null()) list = Json::array();
        for (const auto& peer : peers)
            if (std::find(list.begin(), list.end(), Json(peer)) == list.end()) list.push_back(peer);
        next["enabled"] = true;
        if (profile) next["profile"] = *profile;
    });
}
std::filesystem::path State::transport_directory() const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto profile = data_.at("profile").get<std::string>();
    return profile.empty() ? directory_ : directory_ / "profiles" / profile;
}
void State::bind(const Address& a, const std::string& id) {
    if (!a.valid()) throw std::runtime_error("Invalid channel address");
    std::lock_guard<std::mutex> lock(mu_);
    auto next = data_;
    next["bindings"][a.key()] = {{"address", a.json()}, {"session_id", id}};
    commit(std::move(next));
}
bool State::receipt(const Address& a, const std::string& id) const {
    const auto key = receipt_key(a, id);
    std::lock_guard<std::mutex> lock(mu_);
    const auto& receipts = data_.at("receipts");
    return std::find(receipts.begin(), receipts.end(), key) != receipts.end();
}
void State::remember(const Address& a, const std::string& id) {
    const auto key = receipt_key(a, id);
    std::lock_guard<std::mutex> lock(mu_);
    auto next = data_;
    auto& receipts = next["receipts"];
    if (std::find(receipts.begin(), receipts.end(), key) != receipts.end()) return;
    if (receipts.size() == 4096) receipts.erase(receipts.begin());
    receipts.push_back(key); commit(std::move(next));
}
void State::forget(const Address& a, const std::string& id) {
    const auto key = receipt_key(a, id);
    std::lock_guard<std::mutex> lock(mu_);
    auto next = data_;
    auto& receipts = next["receipts"];
    receipts.erase(std::remove(receipts.begin(), receipts.end(), Json(key)), receipts.end());
    commit(std::move(next));
}
} // namespace acecode::channels
