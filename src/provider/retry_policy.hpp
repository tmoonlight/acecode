#pragma once

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

// Exact billing/quota codes make an otherwise retryable 429 terminal.
bool provider_error_body_has_hard_quota(const std::string& body);

// Shared allowlist for streaming and non-streaming provider requests.
bool provider_http_error_is_retryable(int status_code,
                                      const std::string& body);

// Parses delta-seconds and HTTP-date Retry-After values. Invalid, past, or
// overflowing values return nullopt so callers can use local backoff.
std::optional<std::int64_t> parse_retry_after_ms(
    const std::string& value,
    std::time_t now = std::time(nullptr));

// retry_number is one-based: 1s, 2s, 4s, ... and then 20m forever.
// A valid server delay replaces that attempt's local delay; both paths cap at
// twenty minutes.
std::int64_t provider_retry_delay_ms(
    std::uint64_t retry_number,
    std::optional<std::int64_t> server_delay_ms = std::nullopt);

int saturating_retry_attempt(std::uint64_t retry_number);

// A retry wait that sleeps on a condition variable rather than polling. wake()
// is used by AgentLoop::abort()/shutdown() and can also trigger an immediate
// retry when no abort has been requested.
class ProviderRetryWaiter {
public:
    bool wait_for(std::chrono::milliseconds delay,
                  const std::atomic<bool>* abort_flag);
    void wake();

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::uint64_t wake_generation_ = 0;
};

} // namespace acecode
