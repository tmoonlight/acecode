#pragma once

// LINE Messaging API 客户端:频道访问令牌、机器人信息、webhook 地址、回复 / 推送、
// “正在输入”动画、用户名、配额、附件下载。所有 HTTP 走 im::http_send。
//
// 令牌:默认用 Channel ID + Channel secret 换取无状态令牌(15 分钟有效,到期前 60 秒或
// 遇到 401 时重换,换取过程串行化);用户另外粘贴了长期令牌时直接用它(401 即凭据失效)。
// 线程安全:可在多个线程同时调用。

#include "im/http.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace acecode::im::line {

inline constexpr const char* kApiBase = "https://api.line.me";
inline constexpr const char* kDataApiBase = "https://api-data.line.me";

struct ApiOptions {
    std::string channel_id;
    std::string channel_secret;
    std::string access_token;  // 可选:长期令牌;非空时不再换取无状态令牌
    std::string api_base = kApiBase;
    std::string data_api_base = kDataApiBase;
    bool use_proxy = true;
    std::chrono::milliseconds timeout{std::chrono::seconds(15)};
    // 内容转码(202)时轮询 /content/transcoding 的间隔与次数。
    std::chrono::milliseconds transcoding_poll{std::chrono::seconds(2)};
    int transcoding_attempts = 30;
};

struct ApiResult {
    bool ok = false;           // HTTP 2xx
    long status = 0;           // 0 = 没拿到 HTTP 响应
    nlohmann::json body;       // 解析后的 JSON(没有或无效时为 null)
    std::string message;       // 已脱敏的原因(LINE 的 message / error_description 或网络错误)
    bool auth_failed = false;  // 凭据或令牌被拒(不会自己好)
    bool cancelled = false;
};

struct BotInfo {
    std::string user_id;       // 机器人 userId(Address::account)
    std::string basic_id;      // @xxxx
    std::string display_name;
    std::string picture_url;
    std::string chat_mode;     // chat / bot
};

struct WebhookInfo {
    bool exists = false;       // false:还没设置过 webhook 地址(404)
    std::string endpoint;
    bool active = false;       // “Use webhook” 开关(只能在控制台改)
};

using CancelFn = std::function<bool()>;

class Api {
public:
    explicit Api(ApiOptions options);

    // 有效令牌;失败返回空串,error 为不含凭据的中文原因,auth_failed 表示凭据被拒。
    std::string access_token(std::string* error, bool* auth_failed = nullptr, const CancelFn& cancel = {});
    void invalidate_token();
    bool uses_long_lived_token() const { return !options_.access_token.empty(); }

    ApiResult get(const std::string& path, const CancelFn& cancel = {});
    ApiResult post(const std::string& path, const nlohmann::json& body,
                   const std::vector<std::pair<std::string, std::string>>& headers = {}, const CancelFn& cancel = {});
    ApiResult put(const std::string& path, const nlohmann::json& body, const CancelFn& cancel = {});

    ApiResult bot_info(BotInfo* info, const CancelFn& cancel = {});
    ApiResult webhook_info(WebhookInfo* info, const CancelFn& cancel = {});
    ApiResult set_webhook(const std::string& endpoint, const CancelFn& cancel = {});
    ApiResult test_webhook(const std::string& endpoint, const CancelFn& cancel = {});
    ApiResult reply(const std::string& reply_token, const nlohmann::json& messages);
    // retry_key 用 new_retry_key() 生成;同一批消息重试时必须沿用同一个。
    ApiResult push(const std::string& to, const nlohmann::json& messages, const std::string& retry_key);
    ApiResult start_loading(const std::string& chat_id, int seconds);
    // 私聊取用户资料,群 / 多人聊天取成员资料(只要显示名)。
    ApiResult display_name(const std::string& chat_id, const std::string& user_id, std::string* name);
    ApiResult quota(nlohmann::json* summary, const CancelFn& cancel = {});

    // 下载消息内容(图片 / 视频 / 音频 / 文件);视频音频转码中(202)时等待转码完成。
    // 超过 max_bytes 失败并删除半截文件。
    bool download_content(const std::string& message_id, const std::filesystem::path& dest, std::uint64_t max_bytes,
                          std::string* error, const CancelFn& cancel = {});
    // 外部内容(contentProvider.type = external)的地址:不带鉴权直接下载。
    bool download_url(const std::string& url, const std::filesystem::path& dest, std::uint64_t max_bytes,
                      std::string* error, const CancelFn& cancel = {});

    const ApiOptions& options() const { return options_; }
    // 需要从日志与错误文本里抹掉的值(Channel secret、长期令牌、当前无状态令牌)。
    std::vector<std::string> secrets() const;

private:
    ApiResult call(const std::string& method, const std::string& base, const std::string& path,
                   const std::string& body, const std::vector<std::pair<std::string, std::string>>& headers,
                   const CancelFn& cancel);
    ApiResult parse(const HttpResponse& response) const;
    std::string mint_locked(std::string* error, bool* auth_failed, const CancelFn& cancel);
    bool fetch_content(const std::string& url, const std::filesystem::path& dest, std::uint64_t max_bytes,
                       bool with_auth, HttpResponse* out, const CancelFn& cancel);

    ApiOptions options_;
    mutable std::mutex mu_;
    std::string token_;
    std::chrono::steady_clock::time_point refresh_at_{};
};

// UUID v4(X-Line-Retry-Key);随机数失败时返回空串(调用方不带重试键发送,且不重试)。
std::string new_retry_key();

struct Validation {
    bool ok = false;
    bool network_error = false;  // 网络不通(区别于凭据无效)
    std::string error;           // 中文、一句话、不含凭据
    BotInfo bot;
};

// 保存凭据前的联网校验:Channel ID + secret 换取令牌(填了长期令牌时改用长期令牌),
// 再读 /v2/bot/info。成功时给出机器人 userId、basicId 与名称。
Validation validate_credentials(const ApiOptions& options);

} // namespace acecode::im::line
