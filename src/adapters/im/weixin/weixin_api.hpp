#pragma once

// 微信 iLink HTTP 客户端。
//
// 鉴权请求一律 POST JSON:请求头带 AuthorizationType / X-WECHAT-UIN(每次随机)/ iLink-App-Id /
// iLink-App-ClientVersion / Authorization: Bearer <bot_token>,请求体顶层并入
// base_info.channel_version。响应 Content-Type 是 application/octet-stream,一律按 JSON 解析;
// 应用层错误是 HTTP 200 + ret/errcode 非 0(可能只有 errcode 没有 ret)。
// bot_token 只出现在请求头里,所有错误文本都会脱敏。

#include "im/http.hpp"
#include "im/weixin/weixin_protocol.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace acecode::im::weixin {

struct ApiOptions {
    std::string token;                 // bot_token(扫码登录得到)
    std::string base_url = kApiBase;   // 扫码确认返回的 baseurl
    std::string cdn_base = kCdnBase;   // 媒体 CDN
    bool use_proxy = true;
    std::string channel_version = kChannelVersion;
    std::string bot_agent;             // 非空时随 base_info 上报(仅供平台观测,不参与鉴权)
};

struct ApiResult {
    bool ok = false;
    long status = 0;               // HTTP 状态码;0 = 没拿到响应
    int ret = 0;
    int errcode = 0;
    std::string errmsg;            // 平台原文(已脱敏),只用于日志
    std::string error;             // 给用户看的中文原因(已脱敏)
    nlohmann::json body = nlohmann::json::object();
    bool cancelled = false;
    bool timed_out = false;        // 客户端等到超时(长轮询的正常现象)
    bool session_expired = false;  // -14 等:机器人登录已失效
    bool rate_limited = false;     // -2 限频
};

class Api {
public:
    explicit Api(ApiOptions options);

    // endpoint 是相对路径(如 kEpGetUpdates);body 会被并入 base_info。
    ApiResult post(const std::string& endpoint, nlohmann::json body, std::chrono::milliseconds timeout,
                   std::function<bool()> cancel = {}) const;
    const ApiOptions& options() const { return options_; }

private:
    std::vector<std::pair<std::string, std::string>> headers() const;

    ApiOptions options_;
};

// 扫码相关的未鉴权 GET:只带 iLink-App-Id 与 iLink-App-ClientVersion。
ApiResult get_unauthenticated(const std::string& url, const std::string& channel_version, bool use_proxy,
                              std::chrono::milliseconds timeout, std::function<bool()> cancel = {});

// 把加密后的媒体 POST 到 CDN(必须是 POST,PUT 会 404)。成功 = HTTP 200 且响应头带
// x-encrypted-param(之后作为该文件的 encrypt_query_param 发出去)。
struct CdnUploadResult {
    bool ok = false;
    bool retryable = false;        // 网络错误或 5xx;4xx 不重试
    long status = 0;
    std::string encrypted_param;
    std::string error;             // 中文,已脱敏
};

CdnUploadResult cdn_upload(const std::string& url, const std::string& ciphertext, bool use_proxy,
                           std::chrono::milliseconds timeout);

// 可选的登录状态检查:以 getconfig 探测 bot_token 是否仍有效(不会消耗消息、不移动游标)。
enum class SessionState { Valid, Expired, Unknown };

struct SessionCheck {
    SessionState state = SessionState::Unknown;
    std::string error;  // 中文;Valid 时为空
};

SessionCheck check_session(const ApiOptions& options, const std::string& user_id,
                           std::chrono::milliseconds timeout = std::chrono::seconds(10));

} // namespace acecode::im::weixin
