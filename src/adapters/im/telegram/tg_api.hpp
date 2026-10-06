#pragma once

// Telegram Bot API 客户端(design D11)。请求统一 POST JSON(文件走 multipart),
// 响应按 {"ok","result","error_code","description","parameters.retry_after"} 解析。
// token 出现在 URL 路径里,所有错误与日志文本都会脱敏。

#include "im/http.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace acecode::im::telegram {

inline constexpr const char* kApiBase = "https://api.telegram.org";

struct ApiOptions {
    std::string token;
    std::string api_base = kApiBase;
    bool use_proxy = true;
};

struct ApiResult {
    bool ok = false;
    long status = 0;          // HTTP 状态码;0 = 没拿到响应
    int error_code = 0;       // Bot API error_code
    std::string description;  // 已脱敏
    nlohmann::json result;
    int retry_after = 0;      // 429 时平台要求等待的秒数
    bool cancelled = false;
};

enum class ConflictKind { None, Webhook, OtherPoller };

// 409 有两种:机器人配置了 webhook(getUpdates 不可用),或另一个程序正在轮询同一 token。
ConflictKind conflict_kind(const ApiResult& result);

class Api {
public:
    explicit Api(ApiOptions options);

    ApiResult call(const std::string& method, const nlohmann::json& params,
                   std::chrono::milliseconds timeout = std::chrono::seconds(30),
                   std::function<bool()> cancel = {}) const;
    ApiResult call_multipart(const std::string& method, std::vector<HttpPart> parts,
                             std::chrono::milliseconds timeout = std::chrono::minutes(5)) const;
    // 按 getFile 返回的 file_path 下载;超过 max_bytes 失败并删除半截文件。
    bool download(const std::string& file_path, const std::filesystem::path& dest, std::uint64_t max_bytes,
                  std::string* error) const;
    const ApiOptions& options() const { return options_; }

private:
    std::string base() const;
    ApiResult parse(const HttpResponse& response) const;

    ApiOptions options_;
};

} // namespace acecode::im::telegram
