#pragma once

// Telegram Bot API 传输层(design D11):长轮询收消息(只需出站 HTTPS),
// Markdown → HTML 发送且失败回退纯文本,遵守限速,忙碌时持续“正在输入”。

#include "im/telegram/tg_api.hpp"
#include "im/telegram/tg_rate_limit.hpp"
#include "im/telegram/tg_update.hpp"
#include "im/transport.hpp"
#include "utils/joining_thread.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <vector>

namespace acecode::im::telegram {

struct TelegramTransportOptions {
    ApiOptions api;
    std::int64_t initial_offset = 0;
    // 已处理到的下一个 update offset;调用方持久化,重启后从这里继续,避免重复处理。
    std::function<void(std::int64_t)> on_offset;
    std::chrono::seconds poll_timeout{25};
    std::vector<std::chrono::milliseconds> backoff{
        std::chrono::seconds(1), std::chrono::seconds(2), std::chrono::seconds(5),
        std::chrono::seconds(10), std::chrono::seconds(30), std::chrono::seconds(60)};
    std::chrono::milliseconds conflict_retry{std::chrono::seconds(60)};
    std::chrono::milliseconds typing_interval{4500};
    RateLimiter::Limits limits;
    std::uint64_t max_upload_bytes = 50u * 1024u * 1024u;
    std::uint64_t max_photo_bytes = 10u * 1024u * 1024u;
    std::uint64_t max_download_bytes = 20u * 1024u * 1024u;
};

class TelegramTransport final : public Transport {
public:
    explicit TelegramTransport(TelegramTransportOptions options);
    ~TelegramTransport() override;

    std::string platform() const override { return "telegram"; }
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
    // "remove_webhook":用户确认后移除机器人的 webhook,然后继续长轮询。
    nlohmann::json action(const std::string& name, const nlohmann::json& args) override;

    BotIdentity bot() const;

private:
    void poll_loop();
    void typing_loop();
    bool wait_for(std::chrono::milliseconds duration);
    void set_status(LinkState state, const std::string& detail, bool retry_stopped = false);
    ApiResult send_request(const Address& to, const std::string& method, const nlohmann::json& params,
                           const std::vector<HttpPart>& parts);

    TelegramTransportOptions options_;
    Api api_;
    RateLimiter limiter_;
    TransportCallbacks callbacks_;
    acecode::JoiningThread poller_, typer_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> webhook_cleared_{false};
    std::atomic<std::int64_t> offset_{0};
    std::mutex wake_mu_;
    std::condition_variable wake_;
    mutable std::mutex mu_;  // status_、bot_、typing_
    TransportStatus status_;
    BotIdentity bot_;
    std::set<std::pair<std::string, std::string>> typing_;  // (chat, thread)
};

} // namespace acecode::im::telegram
