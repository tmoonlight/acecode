#include <gtest/gtest.h>

#include "im/telegram/tg_rate_limit.hpp"

// im/telegram/tg_rate_limit:Telegram 限速(同一聊天 1 秒 1 条、群每分钟 20 条、429 暂停)。
// 全部用假时间,不真正等待。

namespace acecode::im::telegram {
namespace {

using Clock = RateLimiter::Clock;
using std::chrono::milliseconds;
using std::chrono::seconds;

// 场景:同一个私聊在同一时刻连续申请三次发送。
// 期望:三次发送时刻依次相隔 1 秒;另一个私聊不受影响,可以立即发送。
TEST(TelegramRateLimit, SpacesMessagesPerChat) {
    RateLimiter limiter;
    const auto t0 = Clock::now();
    EXPECT_EQ(limiter.reserve("a", false, t0), t0);
    EXPECT_EQ(limiter.reserve("a", false, t0), t0 + seconds(1));
    EXPECT_EQ(limiter.reserve("a", false, t0), t0 + seconds(2));
    EXPECT_EQ(limiter.reserve("b", false, t0), t0);
}

// 场景:同一个群在 1 分钟内要发 21 条(相邻间隔设为 0 以单独验证每分钟上限)。
// 期望:前 20 条都在第一分钟内;第 21 条被推迟到第一条发出后满 60 秒。
TEST(TelegramRateLimit, LimitsGroupsToTwentyPerMinute) {
    RateLimiter::Limits limits;
    limits.per_chat_gap = milliseconds(0);
    RateLimiter limiter(limits);
    const auto t0 = Clock::now();
    for (int i = 0; i < 20; ++i) EXPECT_EQ(limiter.reserve("g", true, t0), t0) << i;
    EXPECT_EQ(limiter.reserve("g", true, t0), t0 + seconds(60));
}

// 场景:平台对某聊天返回 429,要求等待 5 秒。
// 期望:该聊天下一次发送不早于 5 秒后;其它聊天不受影响。
TEST(TelegramRateLimit, PenaltyDelaysOnlyThatChat) {
    RateLimiter limiter;
    const auto t0 = Clock::now();
    limiter.penalize("a", seconds(5), t0);
    EXPECT_EQ(limiter.reserve("a", false, t0), t0 + seconds(5));
    EXPECT_EQ(limiter.reserve("b", false, t0), t0);
}

} // namespace
} // namespace acecode::im::telegram
