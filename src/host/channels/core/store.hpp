#pragma once

// 消息通道核心:每个平台一个目录(~/.acecode/channels/<platform>/,design D5)。
//   config.json —— 开关、凭据、机主、授权名单;只在设置页显式操作时写,私有权限原子写入。
//   state.json  —— 会话绑定、每个 IM 会话创建过的会话、去重回执、平台游标;运行时写入。
// 文件结构不合法时 load() 抛异常:拒绝启动该平台并把原因显示在页面上,绝不静默重置。

#include "im/transport.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace acecode::channels::core {

// 一个 IM 会话当前绑定的 ACECode 会话,以及重新打开它所需的身份。
struct BindingRecord {
    im::Address address;
    std::string session_id;
    std::string cwd;
    std::string workspace_hash;
    bool no_workspace = true;
    std::int64_t updated_at_ms = 0;
};

struct AccessEntry {
    std::string principal;  // "user:<id>" / "group:<id>" / "member:<群>:<成员>"
    std::string name;
    std::int64_t approved_at_ms = 0;
};

struct PlatformConfig {
    bool enabled = false;
    nlohmann::json credentials = nlohmann::json::object();
    std::string owner;  // 机主的 principal;空 = 还没有机主
    std::vector<AccessEntry> access;

    bool has_access(const std::string& principal) const;
    const AccessEntry* find_access(const std::string& principal) const;
};

class ChannelStore {
public:
    static constexpr std::size_t kMaxReceipts = 4096;
    static constexpr std::size_t kMaxCreatedPerConversation = 200;

    explicit ChannelStore(std::filesystem::path directory);

    void load();
    const std::filesystem::path& directory() const { return directory_; }

    PlatformConfig config() const;
    // 在锁内修改配置并原子写盘;写盘失败抛异常,内存保持原值。
    void update_config(const std::function<void(PlatformConfig&)>& edit);

    std::optional<BindingRecord> binding(const std::string& address_key) const;
    std::vector<BindingRecord> bindings() const;
    void put_binding(const BindingRecord& record);
    void erase_binding(const std::string& address_key);
    std::vector<std::string> created_sessions(const std::string& address_key) const;
    void remember_created(const std::string& address_key, const std::string& session_id);
    bool has_receipt(const std::string& address_key, const std::string& message_id) const;
    void remember_receipt(const std::string& address_key, const std::string& message_id);
    void forget_receipt(const std::string& address_key, const std::string& message_id);
    std::int64_t cursor(const std::string& name) const;
    void set_cursor(const std::string& name, std::int64_t value);
    // 传输层的其它持久状态(微信的字符串游标、按联系人的上下文令牌等);不存在时返回 null。
    static constexpr std::size_t kMaxValueBytes = 1024 * 1024;
    nlohmann::json value(const std::string& name) const;
    void set_value(const std::string& name, const nlohmann::json& value);
    // 换成另一个机器人账号时:游标与其它持久状态都属于旧机器人,一并清空。
    void clear_transport_state();

private:
    void write_state_locked(nlohmann::json next);

    std::filesystem::path directory_;
    mutable std::mutex mu_;
    PlatformConfig config_;
    nlohmann::json state_;
};

nlohmann::json config_to_json(const PlatformConfig& config);
PlatformConfig config_from_json(const nlohmann::json& value);  // 不合法时抛异常

std::int64_t now_wall_ms();

} // namespace acecode::channels::core
