#pragma once

// 单个平台(QQ / Telegram)的运行时(design D4):
//   - 开关即连:enable 时取该平台账号的跨进程归属锁,取到就连接传输层;
//     取不到进入待命,每隔 standby_retry 重试,持有者退出后接管;disable 立即断开并释放锁。
//   - 入站消息进有界队列,由本平台的工作线程串行交给 Conversations 处理。
//   - 状态变化、新的待批准请求通过 broadcast 推给设置页。

#include "channels/core/access.hpp"
#include "channels/core/conversations.hpp"
#include "channels/core/projection.hpp"
#include "channels/core/store.hpp"
#include "im/transport.hpp"
#include "utils/joining_thread.hpp"
#include "utils/lifetime_token.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace acecode::channels::core {

using TransportFactory =
    std::function<std::shared_ptr<im::Transport>(const std::string& platform, const PlatformConfig&, ChannelStore&)>;
// 校验凭据(联网);成功返回规范化后的凭据,失败返回 null 并写 error。
using CredentialValidator =
    std::function<nlohmann::json(const std::string& platform, const nlohmann::json& credentials, std::string* error)>;
using Broadcast = std::function<void(const std::string& type, const nlohmann::json& payload)>;

struct RuntimeServices {
    ConversationDeps conversation;
    TransportFactory make_transport;
    CredentialValidator validate_credentials;
    Broadcast broadcast;
    std::function<std::int64_t()> current_pid;
    std::chrono::milliseconds standby_retry{std::chrono::seconds(5)};
};

class PlatformRuntime {
public:
    static constexpr std::size_t kMaxQueue = 256;

    PlatformRuntime(std::string platform, std::filesystem::path directory, RuntimeServices services);
    ~PlatformRuntime();
    PlatformRuntime(const PlatformRuntime&) = delete;
    PlatformRuntime& operator=(const PlatformRuntime&) = delete;

    // 读盘;已开启的平台自动连接。
    void start();
    // 断开并停止所有线程,保留配置。
    void stop();

    const std::string& platform() const { return platform_; }
    nlohmann::json snapshot() const;
    std::string owner() const { return store_.config().owner; }

    // 以下操作失败时抛 std::runtime_error(消息可直接展示,不含凭据)。
    void set_enabled(bool enabled);
    // 缺失或为空的字段沿用已保存的值;validate 时先联网校验,失败不保存。
    // 换成另一个机器人账号时,丢弃旧账号下的会话绑定;QQ 的用户 openid 按机器人隔离,
    // 机主与授权名单也一并清空。
    void set_credentials(const nlohmann::json& credentials, bool validate = true);
    void approve(const std::string& request_id, bool approve);
    void revoke(const std::string& principal);
    std::string issue_owner_code();
    std::string issue_owner_pin();  // 6 位绑定码;已有机主时抛异常
    void open_owner_window();
    void set_owner(const std::string& principal, const std::string& name);
    nlohmann::json action(const std::string& name, const nlohmann::json& args);
    im::TransportStatus transport_status() const;

    bool release_session(const std::string& session_id, const std::string& except_key, const std::string& label);
    std::vector<std::string> bound_session_ids() const;

    // 推送当前快照给设置页,并把与上次相比的变化写进日志(state_log.hpp)。
    void publish_state();

private:
    // 以下两个只在持有 ops_mu_ 时调用。
    void connect_locked();
    void disconnect_locked();
    void worker_loop();
    void try_takeover();
    void on_inbound(im::Inbound inbound);
    // 传输层状态变化;source 已不是当前连接(被替换的旧连接)时忽略。
    void on_transport_status(const std::weak_ptr<im::Transport>& source);
    bool configured(const PlatformConfig& config) const;
    std::string account_of(const PlatformConfig& config) const;
    void forget_account_locked(const std::string& account);
    void ensure_loaded() const;

    std::string platform_;
    std::filesystem::path directory_;
    RuntimeServices services_;
    ChannelStore store_;
    mutable AccessControl access_;
    std::shared_ptr<TransportSlot> slot_ = std::make_shared<TransportSlot>();
    std::unique_ptr<Conversations> conversations_;

    std::mutex ops_mu_;      // 串行化开关 / 改凭据 / 接管 / 停止
    std::mutex process_mu_;  // 入站处理与断开互斥:断开后不再处理排队消息
    mutable std::mutex mu_;  // 状态字段
    std::string load_error_;
    bool running_ = false;
    bool owns_lock_ = false;
    bool standby_ = false;
    std::int64_t holder_pid_ = 0;
    std::shared_ptr<im::Transport> transport_;
    struct LockHandle;
    std::unique_ptr<LockHandle> lock_;

    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::deque<im::Inbound> queue_;
    bool restore_pending_ = false;  // 连接后由工作线程为已有绑定恢复出站投影
    std::atomic<bool> stopping_{false};
    std::mutex log_mu_;            // 取快照与比较日志在它下面串行,日志行按状态变化的先后写
    nlohmann::json logged_state_;  // 上次写进日志的快照
    acecode::JoiningThread worker_;
    // 传输层回调经它进入本对象;最后声明,析构时最先撤销(此时 stop() 已停掉传输层)。
    LifetimeToken lifetime_;
};

} // namespace acecode::channels::core
