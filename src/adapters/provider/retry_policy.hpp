#pragma once

// ProviderRetryWaiter 已下沉到 llm/retry_waiter.hpp(P2-02):LlmProvider 的成员类型不能
// 依赖 adapters 层的 provider 模块;这里保留 include 让既有使用方不变。
#include "llm/retry_waiter.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>

namespace acecode {

constexpr std::int64_t kProviderRetryBaseDelayMs = 1000;
constexpr std::int64_t kProviderRetryMaxDelayMs = 20 * 60 * 1000;
// 限流(429 或按文案判定的限流报文)的本地退避上限。限流按固定时间窗口复位
// (实测网关是「100 次 / 10 分钟」),翻倍退避到 20 分钟一次会在窗口早已复位之后
// 还白等十几分钟;每分钟探一次最多多发几个会被秒拒的请求,代价可以忽略。
// 服务端 Retry-After 给出的时长不受这个上限约束,仍以 20 分钟封顶。
constexpr std::int64_t kProviderRateLimitRetryMaxDelayMs = 60 * 1000;

// Exact billing/quota codes make an otherwise retryable 429 terminal.
bool provider_error_body_has_hard_quota(const std::string& body);

// Shared allowlist for streaming and non-streaming provider requests.
// 状态码不可信的网关(src/adapters/pa)按报文文案兜底:写明上下文超限的一律
// 不重试(要走压缩 / 兜底链,重发同一个超大请求没有意义),写明限流的一律
// 重试(与时间窗口有关、与请求大小无关)。
bool provider_http_error_is_retryable(int status_code,
                                      const std::string& body);

// 这条错误是否属于限流:HTTP 429,或报文文案按 PA 适配判定为限流。进度文案与
// 退避上限都按它分支;硬配额(额度用完)不算,那是终止性的。
bool provider_error_is_rate_limited(int status_code, const std::string& body);

// 本地退避的上限:限流用 kProviderRateLimitRetryMaxDelayMs,其它用
// kProviderRetryMaxDelayMs。
std::int64_t provider_retry_max_delay_ms(int status_code, const std::string& body);

// Parses delta-seconds and HTTP-date Retry-After values. Invalid, past, or
// overflowing values return nullopt so callers can use local backoff.
std::optional<std::int64_t> parse_retry_after_ms(
    const std::string& value,
    std::time_t now = std::time(nullptr));

// retry_number is one-based: 1s, 2s, 4s, ... and then max_delay_ms forever
// (default twenty minutes; rate limits pass kProviderRateLimitRetryMaxDelayMs).
// A valid server delay replaces that attempt's local delay and is capped at
// twenty minutes regardless of max_delay_ms.
std::int64_t provider_retry_delay_ms(
    std::uint64_t retry_number,
    std::optional<std::int64_t> server_delay_ms = std::nullopt,
    std::int64_t max_delay_ms = kProviderRetryMaxDelayMs);

int saturating_retry_attempt(std::uint64_t retry_number);


} // namespace acecode
