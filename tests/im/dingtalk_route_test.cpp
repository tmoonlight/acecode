#include <gtest/gtest.h>

#include "im/dingtalk/dingtalk_route.hpp"

// im/dingtalk/dingtalk_route:回复路由(会话 webhook 缓存与合并)、发送限速、有界去重。纯逻辑。

namespace acecode::im::dingtalk {
namespace {

using Clock = RateLimiter::Clock;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr std::int64_t kMargin = 5 * 60 * 1000;

nlohmann::json context(const std::string& webhook, std::int64_t expires, const std::string& staff = "staff01") {
    return {{"sessionWebhook", webhook}, {"sessionWebhookExpiredTime", expires}, {"conversationType", "1"},
            {"conversationId", "cid1"},  {"senderStaffId", staff},               {"robotCode", "dingR"},
            {"msgId", "m1"}};
}

// 场景:从入站消息的回复上下文还原路由;到期时间以字符串形式保存(持久化后可能如此)。
// 期望:各字段都能读出;群聊标记按 conversationType 判断。
TEST(DingTalkRoute, RouteFromContextReadsAllFields) {
    auto ctx = context("https://oapi.dingtalk.com/robot/sendBySession?session=a", 0);
    ctx["sessionWebhookExpiredTime"] = "1700000000000";
    ctx["conversationType"] = "2";
    const auto route = route_from_context(ctx);
    EXPECT_EQ(route.webhook_expires_ms, 1700000000000LL);
    EXPECT_TRUE(route.group);
    EXPECT_EQ(route.staff_id, "staff01");
    EXPECT_EQ(route.robot_code, "dingR");
    EXPECT_EQ(route.conversation_id, "cid1");
    EXPECT_FALSE(route_from_context(nlohmann::json()).group);
}

// 场景:webhook 还有 10 分钟、4 分钟到期,以及没有到期时间。
// 期望:提前 5 分钟视为过期 —— 10 分钟的可用,4 分钟的与没有到期时间的都不可用。
TEST(DingTalkRoute, WebhookUsableHonoursMargin) {
    const std::int64_t now = 1'000'000'000;
    Route route;
    route.webhook = "https://oapi.dingtalk.com/x";
    route.webhook_expires_ms = now + 10 * 60 * 1000;
    EXPECT_TRUE(webhook_usable(route, now, kMargin));
    route.webhook_expires_ms = now + 4 * 60 * 1000;
    EXPECT_FALSE(webhook_usable(route, now, kMargin));
    route.webhook_expires_ms = 0;
    EXPECT_FALSE(webhook_usable(route, now, kMargin));
}

// 场景:触发本回合的旧消息带着快过期的 webhook,而该会话最近一条消息(缓存)带着更新的 webhook;
// 另一种情况是回复上下文为空(Desktop 里输入的回复)。
// 期望:取到期更晚的那个 webhook;上下文为空时直接用缓存,并从缓存补齐 staffId / robotCode。
TEST(DingTalkRoute, ResolvePrefersFresherWebhook) {
    const std::int64_t now = 1'000'000'000;
    RouteBook book;
    auto fresh = route_from_context(context("https://oapi.dingtalk.com/new", now + 80 * 60 * 1000));
    book.remember("k", fresh);
    const auto merged = book.resolve("k", context("https://oapi.dingtalk.com/old", now + 30 * 60 * 1000), now, kMargin);
    EXPECT_EQ(merged.webhook, "https://oapi.dingtalk.com/new");

    const auto empty = book.resolve("k", nlohmann::json::object(), now, kMargin);
    EXPECT_EQ(empty.webhook, "https://oapi.dingtalk.com/new");
    EXPECT_EQ(empty.staff_id, "staff01");
    EXPECT_EQ(empty.robot_code, "dingR");

    const auto unknown = book.resolve("other", nlohmann::json::object(), now, kMargin);
    EXPECT_TRUE(unknown.webhook.empty());
}

// 场景:平台对某个 webhook 报了 session 不存在,而重启后持久化的回复上下文里还带着它。
// 期望:标记失效后 resolve 不再给出它(返回空 webhook,让调用方走 OpenAPI)。
TEST(DingTalkRoute, DeadWebhookIsNeverReused) {
    const std::int64_t now = 1'000'000'000;
    RouteBook book;
    const auto ctx = context("https://oapi.dingtalk.com/gone", now + 60 * 60 * 1000);
    book.remember("k", route_from_context(ctx));
    book.mark_webhook_dead("https://oapi.dingtalk.com/gone");
    EXPECT_TRUE(book.webhook_dead("https://oapi.dingtalk.com/gone"));
    const auto route = book.resolve("k", ctx, now, kMargin);
    EXPECT_TRUE(route.webhook.empty());
    EXPECT_EQ(route.staff_id, "staff01");
}

// 场景:缓存上限 2 条,依次记住 a、b,再更新 a,然后记住 c。
// 期望:淘汰的是最久未更新的 b,而不是最早插入的 a。
TEST(DingTalkRoute, RouteBookEvictsLeastRecentlyUpdated) {
    RouteBook book(2);
    Route route;
    book.remember("a", route);
    book.remember("b", route);
    book.remember("a", route);
    book.remember("c", route);
    EXPECT_EQ(book.size(), 2u);
    EXPECT_TRUE(book.find("a"));
    EXPECT_FALSE(book.find("b"));
    EXPECT_TRUE(book.find("c"));
}

// 场景:同一会话一分钟内发 21 条(钉钉上限每会话每分钟 20 条,超出会被封 10 分钟)。
// 期望:前 20 条立即可发;第 21 条被排到第 1 条之后满 60 秒;其它会话不受影响。
TEST(DingTalkRoute, RateLimiterKeepsTwentyPerMinutePerChat) {
    RateLimiter limiter;
    const auto t0 = Clock::now();
    for (int i = 0; i < 20; ++i) EXPECT_EQ(limiter.reserve("cid", t0), t0);
    EXPECT_EQ(limiter.reserve("cid", t0), t0 + seconds(60));
    EXPECT_EQ(limiter.reserve("other", t0), t0);
}

// 场景:平台报“发送过快”,要求该会话暂停 30 秒。
// 期望:30 秒内的预约都排到暂停结束之后;更短的二次惩罚不会把暂停提前。
TEST(DingTalkRoute, RateLimiterPenaltyDelaysChat) {
    RateLimiter limiter;
    const auto t0 = Clock::now();
    limiter.penalize("cid", seconds(30), t0);
    limiter.penalize("cid", seconds(5), t0);
    EXPECT_EQ(limiter.reserve("cid", t0 + seconds(1)), t0 + seconds(30));
    EXPECT_EQ(limiter.reserve("cid", t0 + seconds(31)), t0 + seconds(31));
}

// 场景:去重集合容量 2,依次见到 x、y、x、z,再查 x。
// 期望:重复的 x 被识别;超出容量后最早的 x 被淘汰,再次出现时视为新消息。
TEST(DingTalkRoute, DedupSetIsBounded) {
    DedupSet seen(2);
    EXPECT_FALSE(seen.seen("x"));
    EXPECT_FALSE(seen.seen("y"));
    EXPECT_TRUE(seen.seen("x"));
    EXPECT_FALSE(seen.seen("z"));
    EXPECT_FALSE(seen.contains("x"));
    EXPECT_TRUE(seen.contains("z"));
}

} // namespace
} // namespace acecode::im::dingtalk
