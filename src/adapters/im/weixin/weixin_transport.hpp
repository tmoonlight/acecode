#pragma once

// 微信 iLink(微信 ClawBot)传输层。
//
//   收:一个线程长轮询 getupdates;游标 get_updates_buf 一变化就先交给调用方持久化,再处理
//       该批消息(至多一次;重复由 id 去重兜底)。-14 等“登录失效”→ Failed 且不再重试,
//       等用户在设置页重新扫码。长消息被平台拆成几段时先合并再交给上层。
//   发:Markdown 转纯文本,按 2000 字分段,每段之间间隔 1.5 秒;带上该用户最近的
//       context_token(没有也照发);context_token 过期被拒时去掉它重发一次;限频时等待后重发,
//       同一段重发沿用同一个 client_id(平台据此去重)。所有发送串行。
//   正在输入:getconfig 取 typing_ticket(缓存 10 分钟),忙碌期间每 5 秒发一次,结束时必须
//       发取消,否则对方一直显示“正在输入”。
//   媒体:见 weixin_media.hpp。
// 只有私聊:Address{platform "weixin", account = ilink_bot_id, chat = sender = 对方 ilink_user_id}。

#include "im/transport.hpp"
#include "im/weixin/weixin_api.hpp"
#include "im/weixin/weixin_protocol.hpp"
#include "utils/joining_thread.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace acecode::im::weixin {

struct WeixinTransportOptions {
    ApiOptions api;               // token = bot_token;base_url = 登录返回的 baseurl
    std::string bot_id;           // ilink_bot_id,作为 Address::account
    std::string initial_cursor;   // 上次持久化的 get_updates_buf;空 = 从头开始
    // 游标变化时回调(在处理该批消息之前),调用方持久化;丢失游标会让平台重放历史消息。
    std::function<void(const std::string&)> on_cursor;
    // 每个对方用户最近的 context_token。主动发送(Desktop 里输入的回复)也要用,调用方持久化;
    // 回调里 token 为空表示该用户的 token 已失效、应删除。
    std::map<std::string, std::string> initial_context_tokens;
    std::function<void(const std::string& peer, const std::string& token)> on_context_token;

    std::chrono::milliseconds poll_timeout{std::chrono::seconds(35)};  // 服务端挂起时长,之后以服务端给的为准
    std::chrono::milliseconds poll_margin{std::chrono::seconds(10)};   // 客户端超时 = 挂起时长 + 余量
    // 长轮询已挂起这么久仍没有报错,说明 token 被接受(无效 token 会立即得到 -14),即视为已连接;
    // 不必等没有新消息时要挂满 35 秒的第一次轮询返回。
    std::chrono::milliseconds connect_grace{std::chrono::seconds(3)};
    std::vector<std::chrono::milliseconds> backoff{
        std::chrono::seconds(1), std::chrono::seconds(2), std::chrono::seconds(5),
        std::chrono::seconds(10), std::chrono::seconds(30), std::chrono::seconds(60)};
    std::chrono::milliseconds chunk_delay{1500};       // 分段之间的间隔(hermes 实测值)
    std::chrono::milliseconds send_retry_delay{1000};  // 网络错误 / 5xx:第 n 次重发前等 n 倍
    std::chrono::milliseconds rate_limit_delay{3000};  // -2 限频:第 n 次重发前等 n 倍
    int send_retries = 4;
    std::chrono::milliseconds typing_interval{5000};
    std::chrono::milliseconds typing_ticket_ttl{std::chrono::minutes(10)};
    std::chrono::milliseconds housekeeping_tick{200};  // 合并片段到期检查与“已连接”判定的周期
    MergeLimits merge;
    std::uint64_t max_upload_bytes = 50u * 1024u * 1024u;
    std::uint64_t max_download_bytes = 50u * 1024u * 1024u;
};

class WeixinTransport final : public Transport {
public:
    explicit WeixinTransport(WeixinTransportOptions options);
    ~WeixinTransport() override;

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

    std::string cursor() const;
    std::map<std::string, std::string> context_tokens() const;

private:
    using Clock = std::chrono::steady_clock;

    struct Ticket {
        std::string value;
        Clock::time_point expires;
    };

    void poll_loop();
    void typing_loop();
    void housekeeping_loop();
    bool wait_for(std::chrono::milliseconds duration);
    void set_status(LinkState state, const std::string& detail, bool retry_stopped = false);
    void promote_if_polling();
    void handle_batch(const nlohmann::json& body);
    void deliver_locked(std::vector<Inbound> ready);
    ApiResult deliver_item(const std::string& peer, const nlohmann::json& item);
    std::string context_token_for(const std::string& peer) const;
    void remember_context_token(const std::string& peer, const std::string& token);
    // 只有当前缓存的仍是 rejected 时才删除(期间收到的新 token 不受影响)。
    void forget_context_token(const std::string& peer, const std::string& rejected);
    std::string typing_ticket(const std::string& peer, bool allow_fetch);
    void send_typing(const std::string& peer, int status, bool allow_fetch);

    WeixinTransportOptions options_;
    Api api_;
    TransportCallbacks callbacks_;
    acecode::JoiningThread poller_, typer_, keeper_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::mutex wake_mu_;
    std::condition_variable wake_;

    mutable std::mutex mu_;  // status_、cursor_、tokens_、tickets_、poll_*
    TransportStatus status_;
    std::string cursor_;
    std::map<std::string, std::string> tokens_;
    std::map<std::string, Ticket> tickets_;
    bool poll_active_ = false;
    Clock::time_point poll_started_{};

    std::mutex deliver_mu_;  // 入站去重、片段合并与交付顺序
    InboundDeduper dedupe_;
    FragmentMerger merger_;

    std::mutex typing_mu_;
    std::condition_variable typing_cv_;
    std::set<std::string> typing_;
    std::set<std::string> typing_stop_;
    std::map<std::string, Clock::time_point> typing_sent_;
    bool typing_dirty_ = false;

    std::mutex send_mu_;  // 出站串行(文字分段、媒体上传都不交错)
};

} // namespace acecode::im::weixin
