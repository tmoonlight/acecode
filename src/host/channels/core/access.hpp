#pragma once

// 消息通道的访问控制与配对(im-channels spec「访问控制与配对」)。
//
// 身份(principal):
//   user:<id>            私聊联系人;用户 id 在平台内唯一的(Telegram、Discord 等),群里同一个人也用它
//   group:<id>           群
//   member:<群>:<成员>   群成员身份按群隔离的平台(QQ 的群成员 openid 与私聊 openid 不同)
// 规则:默认拒绝;未授权私聊只得到配对提示并生成待批准请求(10 分钟过期);
// 群要先批准、且只处理 @机器人 的消息;平台还没有机主时,第一个经扫码 / 一次性链接 /
// 6 位绑定码 / 扫码后窗口确认的私聊联系人成为机主。

#include "channels/core/store.hpp"
#include "im/transport.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace acecode::channels::core {

std::string principal_for(const im::Address& address);
std::string group_principal(const im::Address& address);
std::string principal_kind(const std::string& principal);  // "user" / "group" / "member"

struct PendingRequest {
    std::string id;         // 设置页审批用的短 id
    std::string principal;
    std::string kind;       // user / group / member
    std::string name;       // 展示名(可空)
    im::Address address;
    std::chrono::steady_clock::time_point expires;
};

enum class AccessDecision {
    Allow,          // 交给会话处理
    Ignore,         // 静默忽略
    NotifyPending,  // 回复一次配对提示
};

struct AccessResult {
    AccessDecision decision = AccessDecision::Ignore;
    std::string principal;
    bool owner = false;          // 发送者是机主
    bool claimed_owner = false;  // 这条消息让发送者成为机主,调用方需要持久化
    std::optional<PendingRequest> created;  // 新建的待批准请求,调用方需要通知设置页
    std::string reason;  // 判定原因,只用于日志(设置页不展示这些细节)
    bool consumed = false;  // 这条消息只是绑定码(Telegram /start 或 6 位数字),不交给会话
};

class AccessControl {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::minutes kRequestTtl{10};
    static constexpr std::chrono::minutes kOwnerTtl{10};
    static constexpr std::size_t kMaxPending = 64;

    AccessResult evaluate(const PlatformConfig& config, const im::Inbound& inbound, Clock::time_point now);

    std::vector<PendingRequest> pending(Clock::time_point now);
    // 审批时取走请求;已过期或不存在返回空。
    std::optional<PendingRequest> take(const std::string& id, Clock::time_point now);
    void drop_principal(const std::string& principal);

    static constexpr int kMaxPinFailures = 10;

    // Telegram 机主绑定链接里的一次性码,10 分钟有效。生成失败返回空串。
    std::string issue_owner_code(Clock::time_point now);
    // 6 位数字绑定码,机主私聊机器人发送它即成为机主;10 分钟有效、只能用一次。
    // 有未过期的绑定码时,私聊里猜错 kMaxPinFailures 次就作废全部绑定码(防暴力猜测)。
    std::string issue_owner_pin(Clock::time_point now);
    // QQ 扫码成功但接口没给扫码人时:接下来 10 分钟内第一个私聊的人成为机主。
    void open_owner_window(Clock::time_point now);
    bool owner_window_open(Clock::time_point now) const;

private:
    std::optional<PendingRequest> create_locked(const std::string& principal, const im::Inbound& inbound,
                                                Clock::time_point now);
    bool has_pending_locked(const std::string& principal, Clock::time_point now) const;
    void prune_locked(Clock::time_point now);

    mutable std::mutex mu_;
    std::map<std::string, PendingRequest> requests_;
    std::map<std::string, Clock::time_point> owner_codes_;
    int pin_failures_ = 0;
    Clock::time_point owner_window_until_{};
};

} // namespace acecode::channels::core
