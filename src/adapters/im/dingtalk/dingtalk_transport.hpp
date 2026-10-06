#pragma once

// 钉钉机器人传输层:Stream 模式长连接收消息 + 会话 webhook / 机器人 OpenAPI 发消息。
//
// 线程:
//   - reader_:唯一的 WebSocket 读者兼写者。Stream 注册 → 连接 → 收帧;机器人消息先回执再放进
//     有界队列,绝不在读线程上处理(处理慢会让平台判定连接失效)。
//   - dispatcher_:从队列取消息,按 msgId 去重、记住回复路由、补发暂存输出,再交给核心。
//
// 发送策略(按会话串行,不乱序):
//   1. 会话 webhook 仍有效(到期前 5 分钟视为过期)→ POST Markdown;
//   2. 否则走机器人 OpenAPI:群 → groupMessages/send;单聊且有 staffId → oToMessages/batchSend;
//   3. 组织外用户(只有加密 senderId)且 webhook 已过期,或 OpenAPI 拒绝(权限未开等)→ 暂存,
//      等该会话下一条消息带来新 webhook 时补发。webhook 报 session 不存在时记为失效并改走 OpenAPI。
// 每个会话每分钟最多 20 条;平台报发送过快 / QPS 超限时等待后重发同一条。
//
// 同一应用只能有一条 Stream 连接在收消息(平台会把每条消息随机推给其中一条连接);
// 进程间互斥由核心的通道宿主锁保证。

#include "im/dingtalk/dingtalk_api.hpp"
#include "im/dingtalk/dingtalk_route.hpp"
#include "im/dingtalk/dingtalk_stream.hpp"
#include "im/transport.hpp"
#include "network/websocket_client.hpp"
#include "utils/joining_thread.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <vector>

namespace acecode::im::dingtalk {

struct DingTalkTransportOptions {
    ApiOptions api;
    // 重连等待:min(base·2^n + 抖动, cap);收到过帧的连接断开后从头计数。
    std::chrono::milliseconds backoff_base{std::chrono::seconds(1)};
    std::chrono::milliseconds backoff_cap{std::chrono::seconds(60)};
    bool jitter = true;  // 0~1 秒随机抖动,避免多实例同时重连
    // 连续这么久收不到任何帧(含平台的 SYSTEM ping)就重连;0 = 不检测。
    std::chrono::milliseconds idle_timeout{std::chrono::seconds(120)};
    // Stream 注册与 WebSocket 握手的超时。
    std::chrono::milliseconds open_timeout{std::chrono::seconds(10)};
    // 存活超过这么久的连接被平台要求断开 / 正常关闭后,立即重连而不退避。
    std::chrono::milliseconds healthy_after{std::chrono::seconds(5)};
    RateLimiter::Limits limits;
    // 平台报“发送过快”后的首次等待,之后每次翻倍,最多到 throttle_cap(超限会被封 10 分钟)。
    std::chrono::milliseconds throttle_delay{std::chrono::seconds(60)};
    std::chrono::milliseconds throttle_cap{std::chrono::minutes(10)};
    // 接口 QPS 超限(QpsLimit)时的等待(官方 connector 用 2 秒)。
    std::chrono::milliseconds qps_delay{std::chrono::seconds(2)};
    int max_send_retries = 5;
    // webhook 到期前多久就不再使用。
    std::chrono::milliseconds webhook_margin{std::chrono::minutes(5)};
    std::uint64_t max_upload_bytes = 20u * 1024u * 1024u;
    std::uint64_t max_download_bytes = 25u * 1024u * 1024u;
    // 待处理入站消息上限;满了之后新消息不回执,让平台稍后重投。
    std::size_t max_pending_inbound = 100;
};

class DingTalkTransport final : public Transport {
public:
    explicit DingTalkTransport(DingTalkTransportOptions options);
    ~DingTalkTransport() override;

    std::string platform() const override { return kPlatform; }
    Capabilities capabilities() const override;
    void start(TransportCallbacks callbacks) override;
    void stop() override;
    TransportStatus status() const override;
    SendResult send_text(const Address& to, const std::string& text,
                         const nlohmann::json& reply_context) override;
    SendResult send_file(const Address& to, const std::filesystem::path& path, const std::string& name,
                         const std::string& mime_type, const nlohmann::json& reply_context) override;
    bool download(const Attachment& attachment, const std::filesystem::path& dest,
                  std::string* error) override;

    std::size_t held_count() const;

private:
    using Clock = std::chrono::steady_clock;

    struct HeldItem {
        bool file = false;
        std::string text;
        std::filesystem::path path;
        std::string name;
        std::string mime_type;
    };

    struct Pending {
        std::string message_id;
        std::string data;
    };

    enum class ReadEnd { Stopped, Disconnect, Closed, Idle, Error };

    void run();
    ReadEnd read_loop(std::string* reason);
    void dispatch_loop();
    void handle_message(const Pending& pending);
    bool wait_for(std::chrono::milliseconds duration);
    bool sleep_until(Clock::time_point at);
    void set_status(LinkState state, const std::string& detail, bool retry_stopped = false);
    std::chrono::milliseconds next_backoff(std::size_t attempt);
    std::size_t pending_count();

    std::shared_ptr<std::mutex> chat_lock(const std::string& key);
    bool has_held(const std::string& key) const;
    void push_held(const std::string& key, std::vector<HeldItem> items);
    void flush_held(const Address& to, const nlohmann::json& reply_context);

    SendResult send_text_locked(const Address& to, const std::string& text, const nlohmann::json& reply_context);
    SendResult send_file_locked(const Address& to, const HeldItem& item, const nlohmann::json& reply_context);
    SendResult deliver_markdown(const Address& to, const std::string& markdown, const nlohmann::json& reply_context);
    SendResult deliver_openapi(const Address& to, const Route& route, const std::string& msg_key,
                               const nlohmann::json& msg_param);
    std::string staff_for(const Address& to, const Route& route) const;
    template <typename Send>
    ApiResult throttled(const std::string& chat, Send&& send);

    DingTalkTransportOptions options_;
    Api api_;
    StreamSession session_;
    RouteBook routes_;
    RateLimiter limiter_;
    DedupSet frames_seen_;    // 帧头 messageId
    DedupSet messages_seen_;  // 消息 msgId(重投时不变)
    network::WebSocketClient ws_;
    TransportCallbacks callbacks_;
    std::minstd_rand rng_;    // 只在 reader_ 上使用
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::mutex wake_mu_;
    std::condition_variable wake_;
    mutable std::mutex status_mu_;
    TransportStatus status_;
    std::mutex inbox_mu_;
    std::condition_variable inbox_cv_;
    std::deque<Pending> inbox_;
    mutable std::mutex held_mu_;
    std::map<std::string, std::deque<HeldItem>> held_;
    std::mutex locks_mu_;
    std::map<std::string, std::shared_ptr<std::mutex>> chat_locks_;
    acecode::JoiningThread reader_;
    acecode::JoiningThread dispatcher_;
};

} // namespace acecode::im::dingtalk
