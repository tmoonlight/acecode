#pragma once

// 钉钉开放平台 HTTP 客户端:访问令牌、Stream 注册、会话 webhook / 机器人 OpenAPI 发消息、
// 媒体上传、消息文件下载、凭据校验。
// 线程安全:令牌刷新在锁内单飞(同一时刻只有一个换令牌请求),其余调用可并发。
//
// 两套接口的失败形态不同,都要检查:
//   - 新版 api.dingtalk.com:HTTP 4xx + {"code","message","requestid"};
//   - 旧版 oapi.dingtalk.com 与会话 webhook:HTTP 200 + {"errcode":非 0,"errmsg"}。

#include "im/dingtalk/dingtalk_protocol.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace acecode::im::dingtalk {

struct ApiOptions {
    std::string client_id;      // Client ID(原 AppKey),不是密钥
    std::string client_secret;  // Client Secret(原 AppSecret)
    std::string robot_code;     // 机器人编码;空 = 与 client_id 相同(企业内部应用即如此)
    std::string api_base = kApiBase;
    std::string oapi_base = kOapiBase;
    bool use_proxy = true;
};

struct ApiResult {
    bool ok = false;
    ApiError error;
    nlohmann::json response = nlohmann::json::object();
};

// POST /v1.0/gateway/connections/open 的结果。ticket 一次性、90 秒有效。
struct StreamTicket {
    bool ok = false;
    std::string endpoint;
    std::string ticket;
    ApiError error;
    bool auth_failed = false;  // 401 authFailed:凭据无效,不会自己好
    bool rejected = false;     // 其它 4xx:应用未发布 / 机器人未开 Stream 模式 / IP 白名单
    std::string reason;        // 给用户看的中文原因(已脱敏)
};

// 保存凭据前的联网校验结果。
struct VerifyResult {
    bool ok = false;
    bool auth_failed = false;  // Client ID / Client Secret 不对
    bool rejected = false;     // 凭据对,但钉钉拒绝建立 Stream 连接(未发布、未开 Stream 模式等)
    bool network = false;      // 网络不通
    std::string error;         // 中文一句话,不含凭据
    std::string client_id;
    std::string robot_code;
};

class Api {
public:
    explicit Api(ApiOptions options);

    // 有效令牌;到期前 5 分钟刷新。失败返回空串,error 为不含密钥的原因。
    std::string access_token(std::string* error);
    void invalidate_token();
    // 最近一次换令牌失败是不是因为凭据被拒(而不是网络问题)。
    bool last_token_auth_failed() const;

    // Stream 注册:用 clientId/clientSecret 直接换 endpoint + ticket(不需要令牌)。
    StreamTicket open_stream(std::chrono::milliseconds timeout);

    // 校验凭据:先换令牌,再做一次 Stream 注册(ticket 丢弃,90 秒后自然失效)。
    VerifyResult verify();

    // 往会话 webhook 发一条消息(text / markdown)。URL 不在钉钉域名(或配置的接入地址)下时拒绝。
    ApiResult send_webhook(const std::string& url, const nlohmann::json& body);
    // 机器人单聊:POST /v1.0/robot/oToMessages/batchSend。成功 = 有 processQueryKey 且该用户
    // 不在 invalid / flowControlled / filtered 名单里。msg_param 以 JSON 字符串形式发送。
    ApiResult send_oto(const std::string& robot_code, const std::string& staff_id, const std::string& msg_key,
                       const nlohmann::json& msg_param);
    // 机器人群聊:POST /v1.0/robot/groupMessages/send。成功 = 有 processQueryKey。
    ApiResult send_group(const std::string& robot_code, const std::string& conversation_id,
                         const std::string& msg_key, const nlohmann::json& msg_param);
    // POST oapi /media/upload(multipart,字段 type + media)。成功时 response 含 media_id。
    ApiResult upload_media(const std::string& type, const std::filesystem::path& path, const std::string& name,
                           const std::string& mime_type);
    // downloadCode → 临时下载地址(messageFiles/download)。
    std::string download_url(const std::string& download_code, const std::string& robot_code, std::string* error);
    // 先换下载地址,再不带任何鉴权头和 Content-Type 地 GET(OSS 签名要求);超过 max_bytes 失败并删掉半截文件。
    bool download(const std::string& download_code, const std::string& robot_code,
                  const std::filesystem::path& dest, std::uint64_t max_bytes, std::string* error);

    std::string robot_code() const;
    // 需要从错误文本里抹掉的值:Client Secret 与当前令牌。
    std::vector<std::string> secrets() const;
    const ApiOptions& options() const { return options_; }

private:
    std::string fetch_token_locked(std::string* error);
    ApiResult post_api(const std::string& path, const nlohmann::json& body, std::chrono::milliseconds timeout);
    std::vector<std::string> webhook_bases() const;

    ApiOptions options_;
    mutable std::mutex mu_;
    std::string token_;
    std::chrono::steady_clock::time_point refresh_at_{};
    bool auth_failed_ = false;
    long last_token_status_ = 0;
};

} // namespace acecode::im::dingtalk
