#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace acecode::channels {

using Json = nlohmann::json;
constexpr std::size_t kMaxTextBytes = 64 * 1024;
constexpr std::size_t kMaxMediaBytes = 25 * 1024 * 1024;

struct Address {
    std::string account;
    std::string chat;
    std::string sender;
    bool group = false;

    std::string key() const;
    bool valid() const;
    Json json() const;
    static Address parse(const Json& value);
};

bool valid_peer(const std::string& jid);
bool valid_group(const std::string& jid);
bool path_inside(const std::filesystem::path& path, const std::filesystem::path& root);

// Configuration is independent of owner-written history. A loaded instance keeps
// its configuration snapshot until explicitly changed; corrupt data fails closed.
class State {
public:
    explicit State(std::filesystem::path directory);
    void load();
    void reload_history();
    Json snapshot() const;
    bool enabled() const;
    void set_enabled(bool enabled);
    bool allowed(const Address& address, bool mentioned) const;
    void set_access(const std::string& account, const std::string& jid, bool allow);
    void enable_with_access(const std::string& account, const std::vector<std::string>& peers,
                            const std::optional<std::string>& profile = std::nullopt);
    std::filesystem::path transport_directory() const;
    std::optional<std::string> session(const Address& address) const;
    void bind(const Address& address, const std::string& session_id);
    bool receipt(const Address& address, const std::string& message_id) const;
    void remember(const Address& address, const std::string& message_id);
    void forget(const Address& address, const std::string& message_id);
    const std::filesystem::path& directory() const { return directory_; }

private:
    void commit(Json next);
    void configure(const std::function<void(Json&)>& edit);
    std::filesystem::path directory_;
    mutable std::mutex mu_;
    Json data_;
};

} // namespace acecode::channels
