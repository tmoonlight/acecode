// 自动模式（试验）的拦截计数(openspec add-auto-review-mode,规格「拦截计数与退回确认」
// 「无人可问时的锁定」「模式切换时的状态」)。
// 计数错了的两种后果:该退回弹窗时不退(模型被反复拦截空转),或者无人值守时不锁定
// (每次都调裁决模型,白白烧额度)。

#include <gtest/gtest.h>

#include "permissions/auto_review_denials.hpp"

#include <thread>
#include <vector>

using acecode::auto_review::DenialLimits;
using acecode::auto_review::DenialTracker;

// 场景:裁决连续拦下三次。
// 期望:前两次 record_block 返回 false,第三次返回 true(达到上限 3);此后
// limit_reached() 为真。3 与 20 是与 Claude Code 相同的取值。
TEST(AutoReviewDenials, ConsecutiveLimitIsThree) {
    DenialTracker tracker;
    EXPECT_FALSE(tracker.limit_reached());
    EXPECT_FALSE(tracker.record_block());
    EXPECT_FALSE(tracker.record_block());
    EXPECT_TRUE(tracker.record_block());
    EXPECT_TRUE(tracker.limit_reached());
    EXPECT_EQ(tracker.counts().consecutive, 3);
    EXPECT_EQ(tracker.counts().total, 3);
}

// 场景:拦两次、放行一次、再拦两次(规格「放行清零连续计数」)。
// 期望:放行把连续计数清零,所以后两次不触发上限;累计计数不清零,停在 4。
TEST(AutoReviewDenials, AllowResetsConsecutiveButNotTotal) {
    DenialTracker tracker;
    tracker.record_block();
    tracker.record_block();
    tracker.record_allow();
    EXPECT_FALSE(tracker.record_block());
    EXPECT_FALSE(tracker.record_block());
    EXPECT_FALSE(tracker.limit_reached());
    EXPECT_EQ(tracker.counts().consecutive, 2);
    EXPECT_EQ(tracker.counts().total, 4);
}

// 场景:拦截总是被放行隔开,连续计数永远到不了 3,但累计次数一直涨。
// 期望:第 20 次拦截触发上限。回归意义:只有连续计数的话,一个「拦一次放一次」
// 交替跑偏的会话永远不会回到用户手里。
TEST(AutoReviewDenials, TotalLimitIsTwenty) {
    DenialTracker tracker;
    for (int i = 0; i < 19; ++i) {
        EXPECT_FALSE(tracker.record_block()) << i;
        tracker.record_allow();
    }
    EXPECT_TRUE(tracker.record_block());
    EXPECT_EQ(tracker.counts().total, 20);
}

// 场景:达到上限后退回弹窗,用户作了答。
// 期望:两个计数都清零,之后重新从头数(规格「用户作答后累计计数清零」)。
TEST(AutoReviewDenials, HumanAnswerClearsBothCounts) {
    DenialTracker tracker;
    for (int i = 0; i < 3; ++i) tracker.record_block();
    ASSERT_TRUE(tracker.limit_reached());
    tracker.record_human_answer();
    EXPECT_FALSE(tracker.limit_reached());
    EXPECT_EQ(tracker.counts().consecutive, 0);
    EXPECT_EQ(tracker.counts().total, 0);
}

// 场景:无人可问时锁定;之后同一条用户消息下再来调用,以及用户发来新消息之后。
// 期望:标识没变 → 仍锁定,不解除;标识变了(用户回来了)→ 解除并清零。
// 未锁定时调用 unlock_if_new_user_turn 不产生任何效果。
TEST(AutoReviewDenials, LockHoldsUntilANewUserMessage) {
    DenialTracker tracker;
    EXPECT_FALSE(tracker.unlock_if_new_user_turn("u2"));

    for (int i = 0; i < 3; ++i) tracker.record_block();
    tracker.lock("u1");
    EXPECT_TRUE(tracker.locked());
    EXPECT_FALSE(tracker.unlock_if_new_user_turn("u1"));
    EXPECT_TRUE(tracker.locked());

    EXPECT_TRUE(tracker.unlock_if_new_user_turn("u2"));
    EXPECT_FALSE(tracker.locked());
    EXPECT_EQ(tracker.counts().total, 0);
    EXPECT_FALSE(tracker.limit_reached());
}

// 场景:会话在本模式下被拦两次,用户切到别的模式再切回来(规格「切换清零」)。
// 期望:权限模式的 generation 变了,计数与锁定整体清零;同一个 generation 重复
// 同步不清零(同一段模式内每次裁决前都会调用一次)。
TEST(AutoReviewDenials, ModeGenerationChangeResetsEverything) {
    DenialTracker tracker;
    tracker.sync_mode_generation(7);
    tracker.record_block();
    tracker.record_block();
    tracker.sync_mode_generation(7);
    EXPECT_EQ(tracker.counts().total, 2);

    tracker.lock("u1");
    tracker.sync_mode_generation(9);
    EXPECT_EQ(tracker.counts().consecutive, 0);
    EXPECT_EQ(tracker.counts().total, 0);
    EXPECT_FALSE(tracker.locked());
}

// 场景:测试里用更小的上限构造。
// 期望:上限由构造参数决定,不是写死的。
TEST(AutoReviewDenials, HonoursCustomLimits) {
    DenialTracker tracker(DenialLimits{1, 2});
    EXPECT_TRUE(tracker.record_block());
    tracker.record_allow();
    EXPECT_TRUE(tracker.record_block());   // 累计达到 2
}

// 场景:同一批工具并行执行,多个线程同时记录拦截。
// 期望:没有丢计数 —— 8 个线程各记 250 次,累计恰好 2000。
// 回归意义:计数不加锁时这里会偶发少数,上限判断随之失准。
TEST(AutoReviewDenials, CountsAreThreadSafe) {
    DenialTracker tracker(DenialLimits{1000000, 1000000});
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&tracker] {
            for (int i = 0; i < 250; ++i) tracker.record_block();
        });
    }
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(tracker.counts().total, 2000);
    EXPECT_EQ(tracker.counts().consecutive, 2000);
}
