#include "store.hpp"

#include "utils/atomic_file.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>

namespace acecode::channels::core {
namespace {

constexpr std::uintmax_t kMaxFileBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxBindings = 2048;
constexpr std::size_t kMaxAccess = 1024;

bool safe_text(const std::string& value, std::size_t limit) {
    return value.size() <= limit && value.find('\0') == std::string::npos;
}

bool session_id_ok(const std::string& id) {
    return !id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

bool principal_ok(const std::string& principal) {
    return !principal.empty() && safe_text(principal, 300) &&
           (principal.rfind("user:", 0) == 0 || principal.rfind("group:", 0) == 0 ||
            principal.rfind("member:", 0) == 0);
}

nlohmann::json empty_state() {
    return {{"version", 1}, {"bindings", nlohmann::json::object()}, {"created", nlohmann::json::object()},
            {"receipts", nlohmann::json::array()}, {"cursors", nlohmann::json::object()},
            {"values", nlohmann::json::object()}};
}

nlohmann::json read_json_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) > kMaxFileBytes || ec)
        throw std::runtime_error("通道数据文件过大或不可读:" + path_to_utf8(path.filename()));
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法读取通道数据文件:" + path_to_utf8(path.filename()));
    try {
        return nlohmann::json::parse(in);
    } catch (const std::exception&) {
        throw std::runtime_error("通道数据文件已损坏:" + path_to_utf8(path.filename()));
    }
}

BindingRecord binding_from_json(const nlohmann::json& value) {
    BindingRecord record;
    record.address = im::Address::from_json(value.at("address"));
    record.session_id = value.at("session_id").get<std::string>();
    record.cwd = value.value("cwd", std::string{});
    record.workspace_hash = value.value("workspace_hash", std::string{});
    record.no_workspace = value.value("no_workspace", true);
    record.updated_at_ms = value.value("updated_at_ms", std::int64_t{0});
    return record;
}

nlohmann::json binding_to_json(const BindingRecord& record) {
    return {{"address", record.address.to_json()},
            {"session_id", record.session_id},
            {"cwd", record.cwd},
            {"workspace_hash", record.workspace_hash},
            {"no_workspace", record.no_workspace},
            {"updated_at_ms", record.updated_at_ms}};
}

void validate_state(const nlohmann::json& state) {
    if (!state.is_object() || state.value("version", 0) != 1 || !state["bindings"].is_object() ||
        !state["created"].is_object() || !state["receipts"].is_array() || !state["cursors"].is_object())
        throw std::runtime_error("通道状态文件结构无效");
    if (state["bindings"].size() > kMaxBindings || state["receipts"].size() > ChannelStore::kMaxReceipts)
        throw std::runtime_error("通道状态文件超出上限");
    for (const auto& [key, value] : state["bindings"].items()) {
        const auto record = binding_from_json(value);
        if (!record.address.valid() || record.address.key() != key || !session_id_ok(record.session_id) ||
            !safe_text(record.cwd, 4096) || !safe_text(record.workspace_hash, 128))
            throw std::runtime_error("通道状态文件里的会话绑定无效");
    }
    for (const auto& [key, value] : state["created"].items()) {
        if (!value.is_array() || value.size() > ChannelStore::kMaxCreatedPerConversation || !safe_text(key, 1024))
            throw std::runtime_error("通道状态文件里的会话记录无效");
        for (const auto& id : value)
            if (!id.is_string() || !session_id_ok(id.get<std::string>())) throw std::runtime_error("通道状态文件里的会话记录无效");
    }
    for (const auto& receipt : state["receipts"])
        if (!receipt.is_string() || receipt.get<std::string>().size() > 2048) throw std::runtime_error("通道回执记录无效");
    for (const auto& [key, value] : state["cursors"].items())
        if (!value.is_number_integer() || !safe_text(key, 64)) throw std::runtime_error("通道游标记录无效");
    if (state.contains("values")) {
        if (!state["values"].is_object()) throw std::runtime_error("通道状态文件结构无效");
        for (const auto& [key, value] : state["values"].items()) {
            (void)value;
            if (!safe_text(key, 64)) throw std::runtime_error("通道状态记录无效");
        }
        if (state["values"].dump().size() > ChannelStore::kMaxValueBytes) throw std::runtime_error("通道状态文件超出上限");
    }
}

std::string receipt_key(const std::string& address_key, const std::string& message_id) {
    if (message_id.empty() || message_id.size() > 256) throw std::runtime_error("Invalid channel message id");
    return address_key + "|" + message_id;
}

} // namespace

std::int64_t now_wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool PlatformConfig::has_access(const std::string& principal) const { return find_access(principal) != nullptr; }

const AccessEntry* PlatformConfig::find_access(const std::string& principal) const {
    for (const auto& entry : access)
        if (entry.principal == principal) return &entry;
    return nullptr;
}

nlohmann::json config_to_json(const PlatformConfig& config) {
    nlohmann::json access = nlohmann::json::array();
    for (const auto& entry : config.access)
        access.push_back({{"principal", entry.principal}, {"name", entry.name}, {"approved_at_ms", entry.approved_at_ms}});
    return {{"version", 1},
            {"enabled", config.enabled},
            {"credentials", config.credentials},
            {"owner", config.owner},
            {"access", access}};
}

PlatformConfig config_from_json(const nlohmann::json& value) {
    if (!value.is_object() || value.value("version", 0) != 1 || !value.contains("enabled") ||
        !value["enabled"].is_boolean())
        throw std::runtime_error("通道配置文件结构无效");
    PlatformConfig config;
    config.enabled = value["enabled"].get<bool>();
    config.credentials = value.value("credentials", nlohmann::json::object());
    if (!config.credentials.is_object()) throw std::runtime_error("通道凭据格式无效");
    for (const auto& [key, field] : config.credentials.items())
        if (!field.is_string() || !safe_text(field.get<std::string>(), 512) || !safe_text(key, 64))
            throw std::runtime_error("通道凭据格式无效");
    config.owner = value.value("owner", std::string{});
    if (!config.owner.empty() && !principal_ok(config.owner)) throw std::runtime_error("通道机主记录无效");
    const auto access = value.value("access", nlohmann::json::array());
    if (!access.is_array() || access.size() > kMaxAccess) throw std::runtime_error("通道授权名单无效");
    for (const auto& item : access) {
        AccessEntry entry;
        entry.principal = item.at("principal").get<std::string>();
        entry.name = item.value("name", std::string{});
        entry.approved_at_ms = item.value("approved_at_ms", std::int64_t{0});
        if (!principal_ok(entry.principal) || !safe_text(entry.name, 200)) throw std::runtime_error("通道授权名单无效");
        config.access.push_back(std::move(entry));
    }
    return config;
}

ChannelStore::ChannelStore(std::filesystem::path directory)
    : directory_(std::move(directory)), state_(empty_state()) {}

void ChannelStore::load() {
    PlatformConfig config;
    nlohmann::json state = empty_state();
    const auto config_path = directory_ / "config.json";
    const auto state_path = directory_ / "state.json";
    std::error_code ec;
    if (std::filesystem::exists(config_path, ec)) {
        try {
            config = config_from_json(read_json_file(config_path));
        } catch (const nlohmann::json::exception&) {
            throw std::runtime_error("通道配置文件已损坏");
        }
    }
    if (std::filesystem::exists(state_path, ec)) {
        state = read_json_file(state_path);
        try {
            validate_state(state);
        } catch (const nlohmann::json::exception&) {
            throw std::runtime_error("通道状态文件已损坏");
        }
        if (!state.contains("values")) state["values"] = nlohmann::json::object();  // 早期版本没有这一项
    }
    std::lock_guard<std::mutex> lock(mu_);
    config_ = std::move(config);
    state_ = std::move(state);
}

PlatformConfig ChannelStore::config() const {
    std::lock_guard<std::mutex> lock(mu_);
    return config_;
}

void ChannelStore::update_config(const std::function<void(PlatformConfig&)>& edit) {
    std::lock_guard<std::mutex> lock(mu_);
    auto next = config_;
    edit(next);
    const auto json = config_to_json(next);
    config_from_json(json);  // 写盘前按读盘规则再校验一遍
    std::filesystem::create_directories(directory_);
    if (!atomic_write_file(path_to_utf8(directory_ / "config.json"), json.dump(2) + "\n", true))
        throw std::runtime_error("无法保存通道配置");
    config_ = std::move(next);
}

void ChannelStore::write_state_locked(nlohmann::json next) {
    validate_state(next);
    std::filesystem::create_directories(directory_);
    if (!atomic_write_file(path_to_utf8(directory_ / "state.json"), next.dump(2) + "\n", true))
        throw std::runtime_error("无法保存通道状态");
    state_ = std::move(next);
}

std::optional<BindingRecord> ChannelStore::binding(const std::string& address_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto& bindings = state_["bindings"];
    const auto it = bindings.find(address_key);
    if (it == bindings.end()) return std::nullopt;
    return binding_from_json(*it);
}

std::vector<BindingRecord> ChannelStore::bindings() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<BindingRecord> out;
    for (const auto& [key, value] : state_["bindings"].items()) out.push_back(binding_from_json(value));
    return out;
}

void ChannelStore::put_binding(const BindingRecord& record) {
    std::lock_guard<std::mutex> lock(mu_);
    auto next = state_;
    next["bindings"][record.address.key()] = binding_to_json(record);
    write_state_locked(std::move(next));
}

void ChannelStore::erase_binding(const std::string& address_key) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!state_["bindings"].contains(address_key)) return;
    auto next = state_;
    next["bindings"].erase(address_key);
    write_state_locked(std::move(next));
}

std::vector<std::string> ChannelStore::created_sessions(const std::string& address_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> out;
    const auto it = state_["created"].find(address_key);
    if (it != state_["created"].end())
        for (const auto& id : *it) out.push_back(id.get<std::string>());
    return out;
}

void ChannelStore::remember_created(const std::string& address_key, const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto next = state_;
    auto& list = next["created"][address_key];
    if (!list.is_array()) list = nlohmann::json::array();
    for (const auto& id : list)
        if (id == session_id) return;
    list.push_back(session_id);
    while (list.size() > kMaxCreatedPerConversation) list.erase(list.begin());
    write_state_locked(std::move(next));
}

bool ChannelStore::has_receipt(const std::string& address_key, const std::string& message_id) const {
    const auto key = receipt_key(address_key, message_id);
    std::lock_guard<std::mutex> lock(mu_);
    const auto& receipts = state_["receipts"];
    return std::find(receipts.begin(), receipts.end(), key) != receipts.end();
}

void ChannelStore::remember_receipt(const std::string& address_key, const std::string& message_id) {
    const auto key = receipt_key(address_key, message_id);
    std::lock_guard<std::mutex> lock(mu_);
    auto next = state_;
    auto& receipts = next["receipts"];
    if (std::find(receipts.begin(), receipts.end(), key) != receipts.end()) return;
    receipts.push_back(key);
    while (receipts.size() > kMaxReceipts) receipts.erase(receipts.begin());
    write_state_locked(std::move(next));
}

void ChannelStore::forget_receipt(const std::string& address_key, const std::string& message_id) {
    const auto key = receipt_key(address_key, message_id);
    std::lock_guard<std::mutex> lock(mu_);
    auto next = state_;
    auto& receipts = next["receipts"];
    receipts.erase(std::remove(receipts.begin(), receipts.end(), nlohmann::json(key)), receipts.end());
    write_state_locked(std::move(next));
}

std::int64_t ChannelStore::cursor(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_["cursors"].value(name, std::int64_t{0});
}

void ChannelStore::set_cursor(const std::string& name, std::int64_t value) {
    std::lock_guard<std::mutex> lock(mu_);
    auto next = state_;
    next["cursors"][name] = value;
    write_state_locked(std::move(next));
}

nlohmann::json ChannelStore::value(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto& values = state_["values"];
    const auto it = values.find(name);
    return it == values.end() ? nlohmann::json(nullptr) : *it;
}

void ChannelStore::set_value(const std::string& name, const nlohmann::json& value) {
    if (!safe_text(name, 64)) throw std::runtime_error("Invalid channel state name");
    std::lock_guard<std::mutex> lock(mu_);
    auto next = state_;
    if (value.is_null()) next["values"].erase(name);
    else next["values"][name] = value;
    if (next["values"].dump().size() > kMaxValueBytes) throw std::runtime_error("通道状态超出上限");
    write_state_locked(std::move(next));
}

void ChannelStore::clear_transport_state() {
    std::lock_guard<std::mutex> lock(mu_);
    auto next = state_;
    next["cursors"] = nlohmann::json::object();
    next["values"] = nlohmann::json::object();
    write_state_locked(std::move(next));
}

} // namespace acecode::channels::core
