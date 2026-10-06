#pragma once

// QQ 开放平台 OpenAPI 客户端:访问令牌、网关地址、发消息、富媒体上传、附件下载。
// 线程安全:令牌刷新串行化,发送可并发调用。

#include "im/qqbot/qq_protocol.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace acecode::im::qqbot {

struct ApiOptions {
    std::string app_id;
    std::string app_secret;
    std::string api_base = kApiBase;
    std::string token_url = kTokenUrl;
    bool use_proxy = true;
};

struct ApiResult {
    bool ok = false;
    ApiError error;
    nlohmann::json response = nlohmann::json::object();
};

class Api {
public:
    explicit Api(ApiOptions options);

    // 返回有效令牌,到期前 5 分钟自动刷新。失败返回空串,error 为不含密钥的原因。
    std::string access_token(std::string* error);
    void invalidate_token();
    // 最近一次换令牌失败是不是因为凭据被拒(而不是网络问题)。凭据被拒时不应继续重试。
    bool last_token_auth_failed() const;
    // 强制换取一次令牌,用于保存凭据前的校验。
    bool verify(std::string* error);
    // GET /gateway,返回 wss 地址。
    std::string gateway_url(std::string* error);
    // scope 为 "c2c" 或 "group";target 为对方 openid 或群 openid。
    ApiResult send_message(const std::string& scope, const std::string& target, const nlohmann::json& body);
    ApiResult upload_file(const std::string& scope, const std::string& target, int file_type,
                          const std::string& file_base64, const std::string& file_name);
    // 带鉴权头下载附件;超过 max_bytes 失败并删除半截文件。
    bool download(const std::string& url, const std::filesystem::path& dest, std::uint64_t max_bytes,
                  std::string* error);
    const ApiOptions& options() const { return options_; }

private:
    ApiResult post(const std::string& path, const nlohmann::json& body);
    std::string fetch_token_locked(std::string* error);

    ApiOptions options_;
    mutable std::mutex mu_;
    std::string token_;
    std::chrono::steady_clock::time_point refresh_at_{};
    bool auth_failed_ = false;
};

} // namespace acecode::im::qqbot
