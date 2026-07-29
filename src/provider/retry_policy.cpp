#include "retry_policy.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <limits>

namespace acecode {
namespace {

std::string ascii_lower(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char c) {
            if (c >= 'A' && c <= 'Z') {
                return static_cast<char>(c - 'A' + 'a');
            }
            return static_cast<char>(c);
        });
    return value;
}

std::string trim_ascii(std::string value) {
    const auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    auto first = std::find_if_not(value.begin(), value.end(), is_space);
    auto last = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
    if (first >= last) return {};
    return std::string(first, last);
}

bool contains_any(const std::string& value,
                  std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (value.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool is_hard_quota_code(const std::string& value) {
    const std::string code = ascii_lower(trim_ascii(value));
    return code == "insufficient_quota" ||
           code == "billing_hard_limit_reached" ||
           code == "billing_not_active" ||
           code == "credit_balance_too_low" ||
           code == "hard_limit_reached";
}

bool json_has_hard_quota_code(const nlohmann::json& value) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string key = ascii_lower(it.key());
            if ((key == "code" || key == "type" ||
                 key == "error_code" || key == "reason") &&
                it.value().is_string() &&
                is_hard_quota_code(it.value().get<std::string>())) {
                return true;
            }
            if ((it.value().is_object() || it.value().is_array()) &&
                json_has_hard_quota_code(it.value())) {
                return true;
            }
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            if (json_has_hard_quota_code(item)) return true;
        }
    }
    return false;
}

} // namespace

bool provider_error_body_has_hard_quota(const std::string& body) {
    try {
        const auto parsed = nlohmann::json::parse(body);
        return json_has_hard_quota_code(parsed);
    } catch (const nlohmann::json::parse_error&) {
        return is_hard_quota_code(body);
    }
}

bool provider_http_error_is_retryable(int status_code,
                                      const std::string& body) {
    if (status_code == 429 && provider_error_body_has_hard_quota(body)) {
        return false;
    }
    switch (status_code) {
    case 408:
    case 425:
    case 429:
    case 500:
    case 502:
    case 503:
    case 504:
    case 529:
        return true;
    default:
        break;
    }

    // Authentication, invalid-request, and other client errors remain
    // terminal even if an upstream message happens to mention overload.
    if (status_code >= 400 && status_code < 500) return false;

    const std::string lower = ascii_lower(body);
    return contains_any(
        lower,
        {
            "overloaded_error",
            "\"type\":\"overloaded\"",
            "\"type\": \"overloaded\"",
            "server overloaded",
        });
}

std::optional<std::int64_t> parse_retry_after_ms(
    const std::string& value,
    std::time_t now) {
    const std::string trimmed = trim_ascii(value);
    if (trimmed.empty()) return std::nullopt;

    errno = 0;
    char* end = nullptr;
    const double seconds = std::strtod(trimmed.c_str(), &end);
    if (end && end != trimmed.c_str() && *end == '\0') {
        if (errno == ERANGE || !std::isfinite(seconds) || seconds < 0.0) {
            return std::nullopt;
        }
        const double milliseconds = seconds * 1000.0;
        if (milliseconds >
            static_cast<double>((std::numeric_limits<std::int64_t>::max)())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(milliseconds);
    }

    const std::time_t retry_at = curl_getdate(trimmed.c_str(), &now);
    if (retry_at < 0 || retry_at <= now) return std::nullopt;
    const auto seconds_until = static_cast<std::int64_t>(retry_at - now);
    if (seconds_until >
        (std::numeric_limits<std::int64_t>::max)() / 1000) {
        return std::nullopt;
    }
    return seconds_until * 1000;
}

std::int64_t provider_retry_delay_ms(
    std::uint64_t retry_number,
    std::optional<std::int64_t> server_delay_ms) {
    if (server_delay_ms.has_value() && *server_delay_ms >= 0) {
        return (std::min)(*server_delay_ms, kProviderRetryMaxDelayMs);
    }

    std::int64_t delay = kProviderRetryBaseDelayMs;
    const std::uint64_t doublings =
        retry_number > 0 ? (std::min<std::uint64_t>)(retry_number - 1, 63) : 0;
    for (std::uint64_t i = 0;
         i < doublings && delay < kProviderRetryMaxDelayMs;
         ++i) {
        if (delay > kProviderRetryMaxDelayMs / 2) {
            delay = kProviderRetryMaxDelayMs;
            break;
        }
        delay *= 2;
    }
    return (std::min)(delay, kProviderRetryMaxDelayMs);
}

int saturating_retry_attempt(std::uint64_t retry_number) {
    const auto max_int =
        static_cast<std::uint64_t>((std::numeric_limits<int>::max)());
    return static_cast<int>((std::min)(retry_number, max_int));
}

bool ProviderRetryWaiter::wait_for(
    std::chrono::milliseconds delay,
    const std::atomic<bool>* abort_flag) {
    if (abort_flag && abort_flag->load()) return true;
    if (delay.count() <= 0) return false;

    std::unique_lock<std::mutex> lock(mu_);
    const std::uint64_t observed_generation = wake_generation_;
    cv_.wait_for(lock, delay, [&]() {
        return wake_generation_ != observed_generation ||
               (abort_flag && abort_flag->load());
    });
    return abort_flag && abort_flag->load();
}

void ProviderRetryWaiter::wake() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (wake_generation_ !=
            (std::numeric_limits<std::uint64_t>::max)()) {
            ++wake_generation_;
        } else {
            wake_generation_ = 0;
        }
    }
    cv_.notify_all();
}

} // namespace acecode
