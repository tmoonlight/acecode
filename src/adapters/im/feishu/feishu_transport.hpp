#pragma once

// 飞书 / Lark 传输层:官方长连接(WebSocket + protobuf 帧)收事件,OpenAPI 发消息。
//
// 收:连接前先换令牌、取机器人身份(判定群 @ 用),再请求长连接地址并拨号;每个事件帧
// 先立即 ACK 再解析交给核心;按 message_id 去重,丢弃 30 分钟前的重投。心跳、读超时、
// 拆包重组由 LinkSession 决定;掉线后按服务端的 ReconnectNonce / ReconnectInterval
// 与本地退避重连(每次都重新请求地址,旧地址不复用);凭据无效、连接数超限等致命错误停止重连。
// 停止时发送 close 帧 —— 否则飞书会把消息继续投给已断开的连接(集群模式下随机分发)。
//
// 发:整条消息判定一次是否 Markdown;是则以 post(md 节点,代码块单独成行)发送,被拒时该段
// 改发纯文本;按 3500 字符 / 28 KB 分段;有触发消息时首段走回复接口(话题内每段都走回复,
// 留在话题里),回复目标已撤回则改为直接发到会话;每个接收方 ≤ 4 条/秒,被限流按退避重发,
// 网络错误与 5xx 重发时复用同一个 uuid(平台按 uuid 一小时内去重,不会重复送达)。

#include "im/feishu/feishu_api.hpp"
#include "im/feishu/feishu_gateway.hpp"
#include "im/feishu/feishu_pacer.hpp"
#include "im/transport.hpp"
#include "network/websocket_client.hpp"
#include "utils/joining_thread.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <set>
#include <vector>

namespace acecode::im::feishu {

struct FeishuTransportOptions {
    ApiOptions api;
    // 本地重连退避;拿到服务端 ClientConfig 后与服务端间隔取大。
    std::vector<std::chrono::milliseconds> backoff{
        std::chrono::seconds(1), std::chrono::seconds(2), std::chrono::seconds(5),
        std::chrono::seconds(10), std::chrono::seconds(30), std::chrono::seconds(60)};
    // 被限流(429 / 99991400 / 230020 …)后的等待,逐次递增,与平台响应头 x-ogw-ratelimit-reset
    // 取大(封顶 60 秒);用完仍被限流则发送失败。
    std::vector<std::chrono::milliseconds> rate_limit_backoff{
        std::chrono::seconds(1), std::chrono::seconds(2), std::chrono::seconds(5),
        std::chrono::seconds(10), std::chrono::seconds(30)};
    // 网络错误 / 5xx 的重发等待。
    std::vector<std::chrono::milliseconds> retry_backoff{std::chrono::milliseconds(500),
                                                         std::chrono::milliseconds(1500)};
    std::chrono::milliseconds send_gap{250};                 // 同一接收方相邻两条的最小间隔
    std::chrono::milliseconds receive_poll{500};             // 读循环每次等待帧的时长
    std::chrono::milliseconds bot_refresh_interval{std::chrono::minutes(5)};  // 机器人未就绪时的重查间隔
    LinkSession::Limits link_limits;
    std::uint64_t max_upload_bytes = kMaxFileBytes;
    std::uint64_t max_download_bytes = 50u * 1024u * 1024u;
};

class FeishuTransport final : public Transport {
public:
    explicit FeishuTransport(FeishuTransportOptions options);
    ~FeishuTransport() override;

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

    BotIdentity bot() const;

private:
    enum class Outcome { Failed, Dropped, Fatal, Stopped };
    struct Attempt {
        Outcome outcome = Outcome::Failed;
        std::string message;
    };

    void run();
    Attempt connect_and_serve();
    void serve_connection(Attempt& attempt);
    void handle_event(const std::string& payload);
    void refresh_bot();
    bool wait_for(std::chrono::milliseconds duration);
    void set_status(LinkState state, const std::string& detail, bool retry_stopped = false, bool force = false);
    bool seen_before(const std::string& message_id);
    ApiResult with_retry(const std::string& pace_key, const std::function<ApiResult()>& attempt);
    ApiResult deliver(SendTarget& target, bool reply, const std::string& msg_type, const nlohmann::json& content);
    ApiResult upload_file(const std::filesystem::path& path, const std::string& name, const std::string& mime_type,
                          const FileRoute& route, std::string* key);

    FeishuTransportOptions options_;
    Api api_;
    SendPacer pacer_;
    LinkSession session_;
    network::WebSocketClient ws_;
    TransportCallbacks callbacks_;
    acecode::JoiningThread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    bool server_config_ = false;  // 只在 worker 线程读写
    std::mt19937 rng_;            // 只在 worker 线程使用(重连抖动)
    std::mutex wake_mu_;
    std::condition_variable wake_;
    mutable std::mutex status_mu_;
    TransportStatus status_;
    mutable std::mutex bot_mu_;
    BotIdentity bot_;
    int activate_status_ = -1;
    std::chrono::steady_clock::time_point bot_checked_{};
    std::mutex seen_mu_;
    std::set<std::string> seen_;
    std::deque<std::string> seen_order_;
};

} // namespace acecode::im::feishu
