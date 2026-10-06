#pragma once

// Discord 机器人传输层:网关 WebSocket 收消息 + REST 发消息(只需出站 HTTPS / WSS)。
//
// 线程:
//   - 网关线程:取网关地址、连接、心跳、Identify / Resume、解析 MESSAGE_CREATE 并回调 on_inbound;
//     不做任何会阻塞心跳的 REST 调用。
//   - 输入状态线程:set_typing(on) 期间每 typing_interval(默认 8 秒,指示器 10 秒过期)发一次
//     POST /channels/{id}/typing。
//   - 发送在调用方线程上进行,send_mu_ 串行化,保证同一传输层的消息按调用顺序送达;429 等待后重发。
//
// 登录预算:Identify 每 24 小时最多 1000 次(超出会被重置 token)。优先 Resume;Identify 前检查
// IdentifyBudget 与 /gateway/bot 的 session_start_limit;24 小时计数通过 on_identify_ledger 交给
// 调用方持久化,重启后由 identify_window_start_ms / identify_count 带回。

#include "im/discord/discord_api.hpp"
#include "im/discord/discord_gateway.hpp"
#include "im/transport.hpp"
#include "network/websocket_client.hpp"
#include "utils/joining_thread.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace acecode::im::discord {

struct DiscordTransportOptions {
    ApiOptions api;
    RateLimitPolicy rate_limit;
    std::vector<std::chrono::milliseconds> backoff{
        std::chrono::seconds(1), std::chrono::seconds(2), std::chrono::seconds(5),
        std::chrono::seconds(10), std::chrono::seconds(30), std::chrono::seconds(60)};
    std::chrono::milliseconds rate_limit_close_delay{std::chrono::seconds(60)};  // 网关 4008 之后
    // op 9(d=false)之后随机等待 [min, max] 再重新 Identify。
    std::chrono::milliseconds invalid_session_delay_min{std::chrono::seconds(1)};
    std::chrono::milliseconds invalid_session_delay_max{std::chrono::seconds(5)};
    std::chrono::milliseconds hello_timeout{std::chrono::seconds(20)};
    std::chrono::milliseconds typing_interval{std::chrono::seconds(8)};
    std::chrono::milliseconds receive_slice{250};  // 网关读超时,也是心跳检查的粒度
    IdentifyBudget::Limits identify_limits;
    // 持久化的 24 小时登录计数(Unix 毫秒 / 次数);0 表示没有记录。
    std::int64_t identify_window_start_ms = 0;
    std::int64_t identify_count = 0;
    // 每次 Identify 后在网关线程上调用,交给调用方持久化;必须尽快返回。
    std::function<void(std::int64_t window_start_ms, std::int64_t count)> on_identify_ledger;
    // READY 与大服务器的 GUILD_CREATE 可达数 MB。
    std::size_t max_message_bytes = 16u * 1024u * 1024u;
    std::uint64_t max_upload_bytes = 20u * 1024u * 1024u;   // 官方默认单文件上限 20 MiB
    std::uint64_t max_download_bytes = 25u * 1024u * 1024u;
    // 只从这些主机下载附件(防止把请求引到别处);为空表示不检查(仅测试用)。
    std::vector<std::string> download_hosts{"cdn.discordapp.com", "media.discordapp.net"};
};

class DiscordTransport final : public Transport {
public:
    explicit DiscordTransport(DiscordTransportOptions options);
    ~DiscordTransport() override;

    std::string platform() const override { return kPlatform; }
    Capabilities capabilities() const override;
    void start(TransportCallbacks callbacks) override;
    void stop() override;
    TransportStatus status() const override;
    SendResult send_text(const Address& to, const std::string& text,
                         const nlohmann::json& reply_context) override;
    SendResult send_file(const Address& to, const std::filesystem::path& path, const std::string& name,
                         const std::string& mime_type, const nlohmann::json& reply_context) override;
    void set_typing(const Address& to, bool on) override;
    bool download(const Attachment& attachment, const std::filesystem::path& dest,
                  std::string* error) override;

    // READY 之后的机器人用户 id(= Address::account);连上之前为空。
    std::string bot_user_id() const;

private:
    using Clock = std::chrono::steady_clock;

    struct ConnectionEnd {
        bool fatal = false;
        bool ready = false;  // 本次连接收到过 READY / RESUMED
        std::chrono::milliseconds extra_delay{0};
    };
    struct TypingEntry {
        Address address;
        Clock::time_point due;
    };
    struct AttachmentOrigin {
        std::string channel_id;
        std::string message_id;
    };

    void run();
    ConnectionEnd serve_connection();
    void typing_loop();
    bool wait_for(std::chrono::milliseconds duration);
    void set_status(LinkState state, const std::string& detail, bool retry_stopped = false);
    void on_ready(const ReadyInfo& info);
    void handle_dispatch(const std::string& type, const nlohmann::json& d);
    bool seen_before(const std::string& message_id);
    void remember_attachments(const Inbound& inbound);
    std::string refresh_attachment_url(const std::string& url);
    bool host_allowed(const std::string& url) const;
    std::string resolve_channel(const Address& to, const nlohmann::json& reply_context, std::string* error);
    SendResult oversize_notice(const Address& to, const std::string& name, const nlohmann::json& reply_context,
                               bool local_limit);
    std::int64_t wall_ms() const;
    double random_jitter();
    std::chrono::milliseconds random_between(std::chrono::milliseconds low, std::chrono::milliseconds high);

    DiscordTransportOptions options_;
    Api api_;
    GatewaySession session_;    // 只在网关线程上访问(stop 在 join 之后)
    IdentifyBudget budget_;     // 同上
    std::string gateway_url_;   // 最近一次 /gateway/bot 给出的地址
    std::set<std::string> bot_roles_;  // 机器人的托管角色;只在网关线程上访问
    std::mt19937_64 rng_;              // 只在网关线程上访问
    network::WebSocketClient ws_;
    TransportCallbacks callbacks_;
    acecode::JoiningThread gateway_, typer_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> typing_dirty_{false};
    std::mutex wake_mu_;
    std::condition_variable wake_;
    std::mutex send_mu_;  // 串行化发消息
    mutable std::mutex mu_;  // status_、bot_id_、dm_channels_、typing_、origins_
    TransportStatus status_;
    std::string bot_id_;
    std::map<std::string, std::string> dm_channels_;  // 用户 id → 私聊频道 id
    std::map<std::string, TypingEntry> typing_;       // Address::key() → 条目
    std::map<std::string, AttachmentOrigin> origins_;  // 附件 id → 所在消息(签名过期时重新取地址)
    std::deque<std::string> origin_order_;
    std::mutex seen_mu_;
    std::set<std::string> seen_;
    std::deque<std::string> seen_order_;
};

} // namespace acecode::im::discord
