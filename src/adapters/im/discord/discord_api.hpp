#pragma once

// Discord REST 客户端(API v10)。所有请求带 "Authorization: Bot <token>" 与 DiscordBot 格式的
// User-Agent;429 按响应体的 retry_after(秒,小数)等待后重发,global=true 时暂停全部请求,
// 否则只暂停同一路由。等待可被 cancel() 打断(停机用)。线程安全。
//
// 响应头(X-RateLimit-*)目前拿不到(im::http_send 不返回响应头),所以只能在 429 之后等待,
// 不能提前避让;对单个机器人足够。

#include "im/discord/discord_protocol.hpp"
#include "im/http.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace acecode::im::discord {

struct ApiOptions {
    std::string token;               // 原始 bot token;带 "Bot " 前缀也接受
    std::string api_base = kApiBase;
    bool use_proxy = true;
};

struct RateLimitPolicy {
    int max_retries = 4;  // 同一请求遇到 429 最多重发几次
    // 平台要求的单次等待超过它就不等了,按限流失败返回(避免一条消息卡住几十分钟)。
    std::chrono::milliseconds max_wait{std::chrono::minutes(5)};
    // 网络错误或 5xx 时,发消息(带 nonce,不会重复)最多重发几次。
    int max_send_retries = 2;
    std::chrono::milliseconds send_retry_delay{std::chrono::seconds(1)};
};

struct ApiResult {
    bool ok = false;
    long status = 0;          // HTTP 状态码;0 = 没拿到响应
    ApiError error;           // ok=false 时有效;文本已脱敏
    nlohmann::json body;      // 成功时的响应 JSON;204 / 空响应为 null
    bool cancelled = false;
};

struct DownloadResult {
    bool ok = false;
    long status = 0;
    bool too_large = false;
    bool cancelled = false;
    std::string error;  // 传输层错误(英文,已脱敏;不含 URL 签名)
};

// 去掉首尾空白与可选的 "Bot " 前缀。
std::string normalize_token(std::string token);

class Api {
public:
    explicit Api(ApiOptions options, RateLimitPolicy policy = {});

    const ApiOptions& options() const { return options_; }
    // 规范化后的 token(网关 Identify / Resume 用)。
    const std::string& token() const { return token_; }

    ApiResult get(const std::string& path, std::chrono::milliseconds timeout = std::chrono::seconds(30));
    ApiResult post(const std::string& path, const nlohmann::json& body,
                   std::chrono::milliseconds timeout = std::chrono::seconds(30));
    ApiResult post_multipart(const std::string& path, std::vector<HttpPart> parts,
                             std::chrono::milliseconds timeout = std::chrono::minutes(5));

    ApiResult current_user();         // GET /users/@me
    ApiResult current_application();  // GET /applications/@me
    ApiResult gateway_bot();          // GET /gateway/bot
    ApiResult open_dm(const std::string& user_id);  // POST /users/@me/channels
    ApiResult get_message(const std::string& channel_id, const std::string& message_id);
    ApiResult trigger_typing(const std::string& channel_id);  // POST /channels/{id}/typing
    // POST /channels/{id}/messages。自动加 nonce + enforce_nonce,网络错误或 5xx 时用同一 nonce
    // 重发:请求其实已送达时平台返回原消息,不会出现重复。
    ApiResult create_message(const std::string& channel_id, nlohmann::json body);
    // 同上,multipart:payload_json + files[0]。
    ApiResult create_message_with_file(const std::string& channel_id, nlohmann::json payload,
                                       const std::filesystem::path& file, const std::string& filename,
                                       const std::string& mime_type);
    // 下载 CDN 附件。签名 URL 自带授权,不带 bot token。
    DownloadResult download(const std::string& url, const std::filesystem::path& dest, std::uint64_t max_bytes);

    // 打断限流等待与进行中的请求;之后的请求立即以 cancelled 返回,直到 reset()。
    void cancel();
    void reset();

private:
    using Clock = std::chrono::steady_clock;

    ApiResult perform(HttpRequest request, const std::string& route);
    ApiResult send_with_nonce(const std::string& path, nlohmann::json body, bool multipart,
                              const std::filesystem::path& file, const std::string& filename,
                              const std::string& mime_type);
    bool wait_for_route(const std::string& route);
    bool sleep_for(std::chrono::milliseconds duration);
    void note_rate_limit(const std::string& route, std::chrono::milliseconds wait, bool global);
    std::string url(const std::string& path) const;

    ApiOptions options_;
    RateLimitPolicy policy_;
    std::string token_;
    std::atomic<bool> cancelled_{false};
    std::mutex rl_mu_;
    std::condition_variable rl_cv_;
    Clock::time_point global_until_{};
    std::map<std::string, Clock::time_point> route_until_;
};

struct BotProfile {
    std::string user_id;           // 机器人用户 id(= Address::account)
    std::string username;
    std::string application_id;    // 用于生成邀请链接;GET /applications/@me 失败时为空
    std::string application_name;
    IntentState message_content = IntentState::Unknown;
    std::string invite_url;        // 由 application_id 生成;为空表示拿不到
};

struct ValidationResult {
    bool ok = false;
    bool auth_failed = false;  // token 无效 / 被平台拒绝:换 token 之前不要重试
    std::string error;         // 一句话中文原因,不含 token
    BotProfile profile;
};

// 联网校验 token:GET /users/@me 确认身份,GET /applications/@me 取应用 id 与 intent 开关。
// 后者失败不影响成功(intent 状态为 Unknown)。
ValidationResult validate_credentials(const ApiOptions& options);

} // namespace acecode::im::discord
