#pragma once

// LINE 传输层。LINE 机器人只能用 webhook 收消息,所以连接过程是:
//   1. 用 Channel ID + secret 换令牌(或用长期令牌),读 /v2/bot/info 得到机器人 userId;
//   2. 在 127.0.0.1 上开 LINE 专用回调端口(WebhookListener,Crow 实现由核心注入);
//   3. 取得公网地址:用户自己的 HTTPS 地址,或启动 Cloudflare 快速隧道;
//   4. 经公网地址访问 /line/health 自检,确认请求确实到达本机这个端口;
//   5. 用 PUT /v2/bot/channel/webhook/endpoint 把 <公网地址>/line/webhook 登记给 LINE;
//   6. “Use webhook” 开着才算已连接(这个开关只能在控制台手动打开)。
// 之后持续看守:隧道进程退出或长时间断开就重建隧道并重新登记(快速隧道每次主机名都会变)。
//
// 收消息:回调线程只校验签名、入队并立即回 200(LINE 要求 2 秒内应答);
// 解析、去重、补发暂存输出、交给核心都在传输层自己的事件线程里做。
//
// 发消息:回复令牌新鲜(50 秒内)且没用过时用 reply(免费),否则用 push(计入每月额度);
// 一次最多 5 条;push 因每月额度用完被拒时暂存,等对方下一条消息到来时用它的回复令牌补发。
// LINE 机器人不能发文件:图片经回调端口的临时链接发送,其它文件改发一条文字说明。

#include "im/line/line_api.hpp"
#include "im/line/line_media.hpp"
#include "im/line/line_protocol.hpp"
#include "im/line/line_tunnel.hpp"
#include "im/line/line_webhook.hpp"
#include "im/transport.hpp"
#include "utils/joining_thread.hpp"
#include "utils/lifetime_token.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace acecode::im::line {

struct LineTransportOptions {
    ApiOptions api;
    // 必填:回调端口实现(channels::core::LineWebhookServer)。每个传输层实例独占一个。
    std::shared_ptr<WebhookListener> listener;
    // 用户自己的公网 HTTPS 地址(其反向代理 / 隧道指向 http://127.0.0.1:<listen_port>);
    // 为空时自动启动 Cloudflare 快速隧道。
    std::string public_url;
    std::uint16_t listen_port = 0;  // 0 = 系统分配;填了自己的公网地址时应固定
    // 实际监听端口与 listen_port 不同时回调(核心可持久化,下次沿用)。
    std::function<void(std::uint16_t)> on_listen_port;
    TunnelOptions tunnel;
    bool verify_public_url = true;  // 登记前经公网地址访问 /line/health 自检
    bool test_webhook = true;       // 地址变化后调用 LINE 的 webhook 测试接口(每 10 分钟最多一次)
    bool fetch_profiles = true;     // 取发言人显示名(带缓存)
    std::vector<std::chrono::milliseconds> backoff{
        std::chrono::seconds(1), std::chrono::seconds(2), std::chrono::seconds(5),
        std::chrono::seconds(10), std::chrono::seconds(30), std::chrono::seconds(60)};
    std::chrono::milliseconds public_check_timeout{std::chrono::seconds(60)};
    std::chrono::milliseconds public_check_interval{std::chrono::seconds(2)};
    std::chrono::milliseconds watch_interval{std::chrono::seconds(30)};
    int self_check_every = 10;      // 每 N 次看守做一次公网自检与 webhook 设置复查(约 5 分钟)
    std::chrono::milliseconds webhook_recheck{std::chrono::seconds(60)};  // “Use webhook” 关着时的复查间隔
    std::chrono::milliseconds webhook_test_interval{std::chrono::minutes(10)};
    std::chrono::milliseconds typing_refresh{std::chrono::seconds(50)};
    // 发出消息后动画会消失;仍在忙碌时隔这么久再重显。留出间隔是为了回合结束时
    // (先发最后一条、再关闭“正在输入”)不会在最终答复之后又冒出一次动画。
    std::chrono::milliseconds typing_resume_delay{std::chrono::seconds(2)};
    std::vector<std::chrono::milliseconds> send_retry{
        std::chrono::seconds(1), std::chrono::seconds(2), std::chrono::seconds(4)};
    std::chrono::milliseconds media_ttl{std::chrono::minutes(30)};
    std::uint64_t max_image_bytes = 10u * 1024u * 1024u;
    std::uint64_t max_download_bytes = 25u * 1024u * 1024u;
    std::size_t max_queue = 256;    // 待处理 webhook 上限;满了回 503(开启重投时 LINE 会重发)
};

// 推送被拒后暂存的输出。plain=true 表示 text 已是纯文本分段,补发时不再转换。
struct HeldItem {
    bool file = false;
    bool plain = false;
    std::string text;
    std::filesystem::path path;
    std::string name;
    std::string mime_type;
};

class LineTransport final : public Transport {
public:
    static constexpr std::size_t kMaxHeldPerConversation = 20;

    explicit LineTransport(LineTransportOptions options);
    ~LineTransport() override;

    std::string platform() const override { return kPlatform; }
    Capabilities capabilities() const override;
    void start(TransportCallbacks callbacks) override;
    void stop() override;
    TransportStatus status() const override;
    SendResult send_text(const Address& to, const std::string& text,
                         const nlohmann::json& reply_context) override;
    SendResult send_file(const Address& to, const std::filesystem::path& path, const std::string& name,
                         const std::string& mime_type, const nlohmann::json& reply_context) override;
    // 只对私聊有效(LINE 只允许对用户 id 显示动画);忙碌期间每 50 秒续一次,发出消息后立即重显。
    void set_typing(const Address& to, bool on) override;
    bool download(const Attachment& attachment, const std::filesystem::path& dest,
                  std::string* error) override;

    // 回调端口的请求分发(在回调端口的 HTTP 线程上调用;测试也可直接调用)。
    ListenerReply handle_request(const ListenerRequest& request);
    std::size_t held_count() const;
    std::uint16_t listen_port() const;
    std::string public_base() const;

private:
    enum class LinkStep { Stopped, Fatal, Retry };
    struct QueuedWebhook {
        std::string body;
        std::int64_t received_at_ms = 0;
    };
    struct Piece {
        nlohmann::json message;
        HeldItem source;  // 推送被拒时据此暂存
        std::string text;  // 文字消息的内容(引用识别用);图片为空
    };
    struct RecentMessage {
        std::string text;
        bool from_bot = false;
    };

    // ---- 连接与看守(line_transport.cpp) ----
    void link_loop();
    LinkStep connect_and_watch(std::size_t attempt, bool* reached);
    bool ensure_listener(LinkStep* step);
    bool resolve_public_base(std::size_t attempt, std::string* base, LinkStep* step);
    bool check_public(const std::string& base, std::chrono::milliseconds timeout);
    bool register_webhook(const std::string& desired, WebhookInfo* info, LinkStep* step);
    bool run_webhook_test(const std::string& desired, LinkStep* step);
    LinkStep watch(const std::string& base, const std::string& desired);
    void log_quota();
    void progress(std::size_t attempt, const std::string& detail);
    void set_status(LinkState state, const std::string& detail, bool retry_stopped = false);
    void set_extra(const std::string& key, nlohmann::json value);
    void drop_tunnel();
    bool wait_for(std::chrono::milliseconds duration);
    CancelFn cancel_fn() const;
    ListenerHandler listener_handler();
    std::string account() const;
    bool tunnel_mode() const { return options_.public_url.empty(); }

    // ---- 收消息(line_transport.cpp) ----
    ListenerReply accept_webhook(const std::string& body, const std::string& signature);
    ListenerReply serve_media(const std::string& path, bool head);
    void event_loop();
    void process_webhook(const QueuedWebhook& item);
    void handle_event(WebhookEvent& event);
    bool seen_before(const std::string& key);
    std::string sender_name(const Address& address);
    void remember_message(const std::string& id, const std::string& text, bool from_bot);
    std::optional<RecentMessage> recall_message(const std::string& id);
    void note_reply_token(const Address& address, const nlohmann::json& reply_context);

    // ---- 发消息(line_outbound.cpp) ----
    SendResult deliver_locked(const Address& to, std::vector<HeldItem> items, const nlohmann::json& reply_context,
                              bool flushing);
    std::vector<Piece> build_pieces(const Address& to, const std::vector<HeldItem>& items,
                                    const nlohmann::json& reply_context, bool flushing);
    std::optional<ReplyToken> usable_reply_token_locked(const Address& to, const nlohmann::json& reply_context);
    void mark_token_used_locked(const std::string& token);
    void after_sent(const Address& to, const std::vector<Piece>& pieces, std::size_t begin, std::size_t count,
                    const ApiResult& result);
    void hold(const std::string& key, std::vector<HeldItem> items);
    std::deque<HeldItem> take_held(const std::string& key);
    std::size_t held_size(const std::string& key) const;
    void flush_held(const Address& address, const nlohmann::json& reply_context);
    void send_unidentified_notice(const std::string& reply_token);
    void typing_loop();
    void wake_typer();

    LineTransportOptions options_;
    Api api_;
    CloudflareTunnel tunnel_;
    MediaRegistry media_;
    TransportCallbacks callbacks_;
    std::shared_ptr<std::atomic<bool>> cancel_flag_ = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> typing_wake_{false};
    std::atomic<bool> quota_exhausted_{false};
    std::atomic<std::uint64_t> rejected_signatures_{0};
    std::atomic<std::uint16_t> listener_port_{0};
    std::chrono::steady_clock::time_point last_webhook_test_{};  // 只由连接线程读写
    std::string last_tested_endpoint_;                           // 只由连接线程读写
    acecode::JoiningThread link_, events_, typer_;
    std::mutex wake_mu_;
    std::condition_variable wake_;

    mutable std::mutex mu_;  // 以下状态、身份、公网地址、缓存、正在输入
    TransportStatus status_;
    std::string account_;
    std::string public_base_;
    std::string instance_id_;
    std::map<std::string, ReplyToken> latest_reply_;  // 会话键 → 最近一条入站消息的回复令牌
    std::map<std::string, RecentMessage> recent_;
    std::deque<std::string> recent_order_;
    std::map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>> names_;
    std::map<std::string, std::chrono::steady_clock::time_point> typing_;  // 用户 id → 下次续显时间

    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::deque<QueuedWebhook> queue_;

    std::mutex seen_mu_;
    std::set<std::string> seen_;
    std::deque<std::string> seen_order_;

    std::mutex send_mu_;  // 串行化所有发送,保证同一会话的顺序;以下令牌记录也由它保护
    std::set<std::string> used_tokens_;
    std::deque<std::string> used_order_;

    mutable std::mutex held_mu_;
    std::map<std::string, std::deque<HeldItem>> held_;

    // 最后声明:析构时先撤销,回调端口上还在执行的处理函数结束后才继续析构其它成员。
    acecode::LifetimeToken lifetime_;
};

} // namespace acecode::im::line
