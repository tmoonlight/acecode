#include "provider/retry_policy.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace std::chrono_literals;

namespace {

TEST(ProviderRetryPolicy, ExponentialDelayStartsAtOneSecondAndCapsAtTwentyMinutes) {
    EXPECT_EQ(acecode::provider_retry_delay_ms(1), 1000);
    EXPECT_EQ(acecode::provider_retry_delay_ms(2), 2000);
    EXPECT_EQ(acecode::provider_retry_delay_ms(11), 1024000);
    EXPECT_EQ(acecode::provider_retry_delay_ms(12), 1200000);
    EXPECT_EQ(acecode::provider_retry_delay_ms(1000000), 1200000);
}

TEST(ProviderRetryPolicy, ServerDelayOverridesLocalDelayButUsesSameCap) {
    EXPECT_EQ(acecode::provider_retry_delay_ms(8, 0), 0);
    EXPECT_EQ(acecode::provider_retry_delay_ms(1, 42000), 42000);
    EXPECT_EQ(acecode::provider_retry_delay_ms(1, 3600000), 1200000);
}

TEST(ProviderRetryPolicy, ParsesDeltaSecondsAndHttpDate) {
    EXPECT_EQ(acecode::parse_retry_after_ms("2.5", 0), 2500);
    EXPECT_EQ(
        acecode::parse_retry_after_ms(
            "Thu, 01 Jan 1970 00:01:00 GMT", 0),
        60000);
    EXPECT_FALSE(
        acecode::parse_retry_after_ms(
            "Thu, 01 Jan 1970 00:01:00 GMT", 60)
            .has_value());
    EXPECT_FALSE(acecode::parse_retry_after_ms("not-a-date", 0).has_value());
    EXPECT_FALSE(acecode::parse_retry_after_ms("-1", 0).has_value());
}

TEST(ProviderRetryPolicy, UsesNarrowTransientStatusAllowlist) {
    for (int status : {408, 425, 429, 500, 502, 503, 504, 529}) {
        EXPECT_TRUE(acecode::provider_http_error_is_retryable(status, ""))
            << status;
    }
    for (int status : {0, 400, 401, 403, 404, 409, 422, 501, 505}) {
        EXPECT_FALSE(acecode::provider_http_error_is_retryable(status, ""))
            << status;
    }
    EXPECT_TRUE(acecode::provider_http_error_is_retryable(
        0, R"({"type":"overloaded_error"})"));
    EXPECT_TRUE(acecode::provider_http_error_is_retryable(
        200, R"({"type":"overloaded_error"})"));
    EXPECT_FALSE(acecode::provider_http_error_is_retryable(
        401, R"({"type":"overloaded_error"})"));
}

TEST(ProviderRetryPolicy, HardQuotaMakesRateLimitTerminal) {
    EXPECT_FALSE(acecode::provider_http_error_is_retryable(
        429, R"({"error":{"code":"insufficient_quota"}})"));
    EXPECT_FALSE(acecode::provider_http_error_is_retryable(
        429, R"({"error":{"code":"billing_hard_limit_reached"}})"));
    EXPECT_TRUE(acecode::provider_http_error_is_retryable(
        429, R"({"error":{"code":"rate_limit_exceeded"}})"));
    EXPECT_TRUE(acecode::provider_http_error_is_retryable(
        429,
        R"({"error":{"message":"gateway did not return insufficient_quota"}})"));
}

TEST(ProviderRetryPolicy, RetryWaitWakesPromptlyOnAbort) {
    acecode::ProviderRetryWaiter waiter;
    std::atomic<bool> abort{false};

    auto future = std::async(std::launch::async, [&]() {
        return waiter.wait_for(std::chrono::minutes(20), &abort);
    });
    std::this_thread::sleep_for(20ms);
    abort.store(true);
    waiter.wake();

    const auto status = future.wait_for(500ms);
    if (status != std::future_status::ready) {
        waiter.wake();
    }
    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_TRUE(future.get());
}

TEST(ProviderRetryPolicy, WakeWithoutAbortStartsNextAttempt) {
    acecode::ProviderRetryWaiter waiter;
    std::atomic<bool> abort{false};

    auto future = std::async(std::launch::async, [&]() {
        return waiter.wait_for(std::chrono::minutes(20), &abort);
    });
    std::this_thread::sleep_for(20ms);
    waiter.wake();

    const auto status = future.wait_for(500ms);
    if (status != std::future_status::ready) {
        abort.store(true);
        waiter.wake();
        (void)future.wait_for(500ms);
    }
    ASSERT_EQ(status, std::future_status::ready);
    EXPECT_FALSE(future.get());
}

} // namespace
