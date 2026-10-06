#include <gtest/gtest.h>

#include "im/qqbot/qq_reply_budget.hpp"

// im/qqbot/qq_reply_budget:QQ 被动回复额度(私聊 60 分钟 4 条、群聊 5 分钟 5 条)、
// 全局递增 msg_seq,以及主动消息被拒时的补发队列。

namespace acecode::im::qqbot {
namespace {

nlohmann::json context(const std::string& id, const std::string& scope, std::int64_t at) {
    return {{"msg_id", id}, {"scope", scope}, {"received_at_ms", at}};
}

constexpr std::int64_t kMinute = 60 * 1000;

// 场景:同一条私聊消息连续回复 5 次。
// 期望:前 4 次被动回复(带 msg_id),第 5 次改为主动消息。
TEST(QqReplyBudget, PrivateChatAllowsFourPassiveReplies) {
    ReplyBudget budget;
    const auto ctx = context("M1", "c2c", 0);
    for (int i = 0; i < 4; ++i) {
        const auto plan = budget.plan(ctx, kMinute);
        EXPECT_TRUE(plan.passive) << i;
        EXPECT_EQ(plan.msg_id, "M1");
    }
    EXPECT_FALSE(budget.plan(ctx, kMinute).passive);
}

// 场景:私聊消息 61 分钟后才有回复;群消息 6 分钟后才有回复。
// 期望:都已超出被动回复窗口,改为主动消息。
TEST(QqReplyBudget, WindowExpiryForcesActiveMessage) {
    ReplyBudget budget;
    EXPECT_FALSE(budget.plan(context("M1", "c2c", 0), 61 * kMinute).passive);
    EXPECT_TRUE(budget.plan(context("M2", "group", 0), 4 * kMinute).passive);
    EXPECT_FALSE(budget.plan(context("M3", "group", 0), 6 * kMinute).passive);
}

// 场景:同一条群消息连续回复 6 次。
// 期望:前 5 次被动,第 6 次主动。
TEST(QqReplyBudget, GroupAllowsFivePassiveReplies) {
    ReplyBudget budget;
    const auto ctx = context("G1", "group", 0);
    for (int i = 0; i < 5; ++i) EXPECT_TRUE(budget.plan(ctx, kMinute).passive) << i;
    EXPECT_FALSE(budget.plan(ctx, kMinute).passive);
}

// 场景:平台拒绝了某条消息的被动回复(例如窗口其实已过);以及没有回复上下文的发送。
// 期望:该消息此后不再尝试被动;没有上下文时直接主动。
TEST(QqReplyBudget, ExhaustAndMissingContextUseActiveMessages) {
    ReplyBudget budget;
    const auto ctx = context("M1", "c2c", 0);
    EXPECT_TRUE(budget.plan(ctx, kMinute).passive);
    budget.exhaust("M1");
    EXPECT_FALSE(budget.plan(ctx, kMinute).passive);
    EXPECT_FALSE(budget.plan(nlohmann::json::object(), kMinute).passive);
}

// 场景:交替为不同消息安排发送,并单独申请序号。
// 期望:msg_seq 全局严格递增,不会出现重复(平台按 msg_id + msg_seq 去重)。
TEST(QqReplyBudget, SequenceIsGloballyIncreasing) {
    ReplyBudget budget;
    std::int64_t last = 0;
    for (int i = 0; i < 10; ++i) {
        const auto plan = budget.plan(context(i % 2 ? "A" : "B", "c2c", 0), kMinute);
        EXPECT_GT(plan.msg_seq, last);
        last = plan.msg_seq;
        const auto seq = budget.next_seq();
        EXPECT_GT(seq, last);
        last = seq;
    }
}

// 场景:主动消息被拒后为同一会话暂存 22 条输出,另一个会话暂存 1 条。
// 期望:每个会话最多保留最新 20 条(丢弃最旧的 2 条),取出时保持原顺序且取后清空;
// 不同会话互不影响。
TEST(QqHeldQueue, KeepsNewestTwentyInOrderPerConversation) {
    HeldQueue queue;
    std::size_t dropped = 0;
    for (int i = 0; i < 22; ++i) {
        HeldItem item;
        item.text = std::to_string(i);
        dropped += queue.push("a", item);
    }
    HeldItem other;
    other.text = "other";
    queue.push("b", other);
    EXPECT_EQ(dropped, 2u);
    EXPECT_EQ(queue.size("a"), 20u);
    EXPECT_EQ(queue.total(), 21u);
    const auto items = queue.take("a");
    ASSERT_EQ(items.size(), 20u);
    EXPECT_EQ(items.front().text, "2");
    EXPECT_EQ(items.back().text, "21");
    EXPECT_EQ(queue.size("a"), 0u);
    EXPECT_EQ(queue.size("b"), 1u);
}

} // namespace
} // namespace acecode::im::qqbot
