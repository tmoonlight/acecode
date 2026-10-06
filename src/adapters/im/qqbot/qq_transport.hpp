#pragma once

// QQ 官方机器人传输层:本机 WebSocket 网关长连接 + OpenAPI 发送(design D10)。
//
// 发送策略:先用触发本回合的那条消息做被动回复;额度用尽、窗口已过或平台拒绝被动回复时
// 改发主动消息;主动消息也被拒(对方/群主关闭、额度用完)时暂存,等该会话下一条消息到来时
// 作为被动回复补发。某会话已有暂存时,新的输出直接排在暂存之后,保证顺序。

#include "im/qqbot/qq_api.hpp"
#include "im/qqbot/qq_gateway.hpp"
#include "im/qqbot/qq_reply_budget.hpp"
#include "im/transport.hpp"
#include "network/websocket_client.hpp"
#include "utils/joining_thread.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <vector>

namespace acecode::im::qqbot {

struct QqTransportOptions {
    ApiOptions api;
    bool markdown = true;
    ReplyLimits limits;
    std::vector<std::chrono::milliseconds> backoff{
        std::chrono::seconds(1), std::chrono::seconds(2), std::chrono::seconds(5),
        std::chrono::seconds(10), std::chrono::seconds(30), std::chrono::seconds(60)};
    std::chrono::milliseconds rate_limit_delay{std::chrono::seconds(60)};
    std::uint64_t max_upload_bytes = 20u * 1024u * 1024u;
    std::uint64_t max_download_bytes = 25u * 1024u * 1024u;
};

class QqTransport final : public Transport {
public:
    explicit QqTransport(QqTransportOptions options);
    ~QqTransport() override;

    std::string platform() const override { return "qq"; }
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

    std::size_t held_count() const { return held_.total(); }

private:
    void run();
    bool wait_for(std::chrono::milliseconds duration);
    void set_status(LinkState state, const std::string& detail, bool retry_stopped = false);
    void handle_dispatch(const std::string& type, const nlohmann::json& d);
    bool seen_before(const std::string& message_id);
    SendResult deliver(const Address& to, const HeldItem& item, const nlohmann::json& reply_context,
                       bool flushing);
    ApiResult attempt(const Address& to, const HeldItem& item, const ReplyPlan& plan);
    ApiResult attempt_text(const std::string& scope, const std::string& target, const std::string& text,
                           const ReplyPlan& plan);

    QqTransportOptions options_;
    Api api_;
    ReplyBudget budget_;
    HeldQueue held_;
    GatewaySession session_;
    network::WebSocketClient ws_;
    TransportCallbacks callbacks_;
    acecode::JoiningThread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> markdown_{true};
    std::mutex wake_mu_;
    std::condition_variable wake_;
    mutable std::mutex status_mu_;
    TransportStatus status_;
    std::mutex seen_mu_;
    std::set<std::string> seen_;
    std::deque<std::string> seen_order_;
};

} // namespace acecode::im::qqbot
