#include <gtest/gtest.h>

#include "pa/pa_overflow_rescue.hpp"
#include <limits>

namespace pa = acecode::pa;

namespace {

// 取下一步并推进状态,与 AgentLoop::run_pa_overflow_rescue 的调用顺序一致。
pa::RescuePlan step(pa::RescueState& state, const pa::RescueInputs& inputs) {
    const pa::RescuePlan plan = pa::next_rescue_step(state, inputs);
    pa::advance_rescue_state(state, plan);
    return plan;
}

pa::RescueInputs inputs(int request_tokens,
                        int history_tokens,
                        bool emergency_profile = false) {
    pa::RescueInputs in;
    in.request_tokens = request_tokens;
    in.history_tokens = history_tokens;
    in.emergency_profile = emergency_profile;
    return in;
}

// 等待缩放系数是进程级的,用例之间必须还原。
class WaitScaleGuard {
public:
    explicit WaitScaleGuard(double scale) { pa::set_rescue_wait_scale_for_test(scale); }
    ~WaitScaleGuard() { pa::set_rescue_wait_scale_for_test(1.0); }
};

} // namespace

// 触发场景:正常档请求第一次被服务端以「请求上下文过大」拒收。
// 期望行为:先原样重发两次(2 秒、5 秒),期间不动历史、不给学习器记账 ——
// 抽风不是证据,记进去会白白收紧压缩窗口。
TEST(PaOverflowRescue, RetriesTheSameRequestTwiceBeforeShrinking) {
    pa::RescueState state;

    const auto first = step(state, inputs(100000, 60000));
    EXPECT_EQ(first.action, pa::RescueAction::RetrySameRequest);
    EXPECT_EQ(first.wait_ms, 2000);
    EXPECT_FALSE(first.record_rejection);

    const auto second = step(state, inputs(100000, 60000));
    EXPECT_EQ(second.action, pa::RescueAction::RetrySameRequest);
    EXPECT_EQ(second.wait_ms, 5000);
    EXPECT_FALSE(second.record_rejection);

    EXPECT_TRUE(state.active);
    EXPECT_EQ(state.same_request_retries, 2);
    EXPECT_FALSE(state.rejection_recorded);
}

// 触发场景:两次原样重发都被拒,拒收已经确认。
// 期望行为:第三步收缩历史,目标 = 请求的 85% 再扣掉历史之外的固定部分
// (100000 × 85% = 85000,固定 40000,历史目标 45000);此时才记一次账。
TEST(PaOverflowRescue, ShrinksToEightyFivePercentAfterRetriesFail) {
    pa::RescueState state;
    step(state, inputs(100000, 60000));
    step(state, inputs(100000, 60000));

    const auto plan = step(state, inputs(100000, 60000));
    EXPECT_EQ(plan.action, pa::RescueAction::ShrinkHistory);
    EXPECT_EQ(plan.target_history_tokens, 45000);
    EXPECT_TRUE(plan.record_rejection);
    EXPECT_TRUE(state.rejection_recorded);
    EXPECT_EQ(state.shrink_rounds, 1);
}

// 触发场景:收缩过一档之后重发仍被拒。
// 期望行为:继续收缩下一档(85000 × 85% = 72250,固定 40000 → 32250),不再
// 原样重发,也不再记账 —— 一个 episode 只记最初那次确认拒收的规模,否则一串
// 越来越小的被拒规模会把压缩窗口一路砍到远小于真实上限。
TEST(PaOverflowRescue, KeepsShrinkingWithoutFurtherRetriesOrRecording) {
    pa::RescueState state;
    step(state, inputs(100000, 60000));
    step(state, inputs(100000, 60000));
    step(state, inputs(100000, 60000));

    const auto plan = step(state, inputs(85000, 45000));
    EXPECT_EQ(plan.action, pa::RescueAction::ShrinkHistory);
    EXPECT_EQ(plan.target_history_tokens, 32250);
    EXPECT_FALSE(plan.record_rejection);
    EXPECT_EQ(state.shrink_rounds, 2);
    EXPECT_EQ(state.same_request_retries, 2);
}

// 触发场景:收缩已经腾不出任何空间(老回合只剩一组、工具输出也清完了,
// 调用方置 shrink_exhausted)。
// 期望行为:改用紧急档;若这一轮还没记过账(收缩一次都没成功过),此时补记
// —— 原样重发失败已经证实拒收是真的。
TEST(PaOverflowRescue, FallsBackToEmergencyProfileWhenNothingShrinks) {
    pa::RescueState state;
    step(state, inputs(60000, 2000));
    step(state, inputs(60000, 2000));
    const auto shrink = step(state, inputs(60000, 2000));
    ASSERT_EQ(shrink.action, pa::RescueAction::ShrinkHistory);
    // 调用方发现修剪没有任何改动:
    state.shrink_exhausted = true;

    const auto plan = step(state, inputs(60000, 2000));
    EXPECT_EQ(plan.action, pa::RescueAction::EmergencyProfile);
    // 收缩那一步已经记过账,紧急档不再重复记。
    EXPECT_FALSE(plan.record_rejection);
    EXPECT_TRUE(state.rejection_recorded);
}

// 触发场景:紧急档请求也被拒 —— 请求已经不能再小了。
// 期望行为:按退避等待后原样重发:5s、10s、20s、40s、60s、60s…,12 次之后
// 才放弃;紧急档的被拒规模永远不记账(它没有工具定义与注入上下文,规模不代表
// 正常请求能过的上限)。
TEST(PaOverflowRescue, WaitsWithBackoffAndGivesUpOnlyAfterTheCap) {
    pa::RescueState state;
    state.shrink_exhausted = true;
    state.rejection_recorded = true;

    const int expected_delays[] = {5000, 10000, 20000, 40000, 60000, 60000};
    for (int i = 0; i < pa::PA_RESCUE_MAX_WAIT_RETRIES; ++i) {
        const auto plan = step(state, inputs(50000, 500, /*emergency=*/true));
        ASSERT_EQ(plan.action, pa::RescueAction::WaitAndRetry) << "attempt " << i;
        EXPECT_FALSE(plan.record_rejection);
        const int expected = i < 6 ? expected_delays[i] : 60000;
        EXPECT_EQ(plan.wait_ms, expected) << "attempt " << i;
    }
    EXPECT_EQ(state.wait_retries, pa::PA_RESCUE_MAX_WAIT_RETRIES);

    const auto final_plan = step(state, inputs(50000, 500, /*emergency=*/true));
    EXPECT_EQ(final_plan.action, pa::RescueAction::GiveUp);
}

// 触发场景:同一回合里紧急档已经粘住(上一轮 episode 用过),新的工具输出
// 进来后紧急档请求又被拒 —— 这是新一轮 episode,历史还没缩过。
// 期望行为:紧急档被拒不是抽风该有的样子(紧急档已经很小),跳过原样重发
// 直接收缩;紧急档规模不记账。
TEST(PaOverflowRescue, EmergencyRejectionSkipsSameRequestRetriesAndRecording) {
    pa::RescueState state;

    const auto plan = step(state, inputs(40000, 30000, /*emergency=*/true));
    EXPECT_EQ(plan.action, pa::RescueAction::ShrinkHistory);
    EXPECT_EQ(plan.target_history_tokens, 24000);
    EXPECT_FALSE(plan.record_rejection);
    EXPECT_FALSE(state.rejection_recorded);
    EXPECT_EQ(state.same_request_retries, 0);
}

// 触发场景:历史很小,请求几乎全是固定部分(system prompt + 注入上下文),
// 85% 目标比固定部分还小。
// 期望行为:历史目标钳到 1,让修剪层把能清的全清掉,而不是算出负数。
TEST(PaOverflowRescue, HistoryTargetClampsToOneWhenFixedContextDominates) {
    pa::RescueState state;
    state.same_request_retries = pa::PA_RESCUE_SAME_REQUEST_RETRIES;

    const auto plan = step(state, inputs(80000, 1000));
    ASSERT_EQ(plan.action, pa::RescueAction::ShrinkHistory);
    EXPECT_EQ(plan.target_history_tokens, 1);
}

// 触发场景:单测把等待缩放系数设为 0。
// 期望行为:所有等待变成 0ms;还原成 1.0 后恢复原值。退避表本身不受影响。
TEST(PaOverflowRescue, WaitScaleZeroDisablesWaiting) {
    {
        WaitScaleGuard guard(0.0);
        EXPECT_EQ(pa::scaled_rescue_wait_ms(5000), 0);
        EXPECT_EQ(pa::scaled_rescue_wait_ms(60000), 0);
    }
    EXPECT_EQ(pa::scaled_rescue_wait_ms(5000), 5000);
    EXPECT_EQ(pa::wait_retry_delay_ms(0), 5000);
    EXPECT_EQ(pa::wait_retry_delay_ms(3), 40000);
    EXPECT_EQ(pa::wait_retry_delay_ms(4), 60000);
    EXPECT_EQ(pa::wait_retry_delay_ms(50), 60000);
    EXPECT_EQ(pa::same_request_retry_delay_ms(0), 2000);
    EXPECT_EQ(pa::same_request_retry_delay_ms(1), 5000);
}

TEST(PaOverflowRescue, SmallRequestsWaitIndefinitelyWithoutShrinkingOrLearning) {
    pa::RescueState state;
    const int delays[] = {5000, 10000, 20000, 40000};
    for (int i = 0; i < 40; ++i) {
        const auto plan = step(state, inputs(39999, 30000));
        EXPECT_EQ(plan.action, pa::RescueAction::WaitAndRetry);
        EXPECT_TRUE(plan.preserve_context);
        EXPECT_FALSE(plan.record_rejection);
        EXPECT_EQ(plan.target_history_tokens, 0);
        EXPECT_EQ(plan.wait_ms, i < 4 ? delays[i] : 60000);
    }
    EXPECT_EQ(state.shrink_rounds, 0);
    EXPECT_EQ(state.same_request_retries, 0);
    state.wait_retries = std::numeric_limits<int>::max();
    EXPECT_EQ(step(state, inputs(20000, 10000)).action, pa::RescueAction::WaitAndRetry);
    EXPECT_EQ(state.wait_retries, std::numeric_limits<int>::max());
}

TEST(PaOverflowRescue, LargeRecoveryStopsShrinkingOnceRequestFallsBelowFloor) {
    pa::RescueState state;
    state.same_request_retries = pa::PA_RESCUE_SAME_REQUEST_RETRIES;
    EXPECT_EQ(step(state, inputs(40000, 30000)).action, pa::RescueAction::ShrinkHistory);
    const auto plan = step(state, inputs(34000, 24000));
    EXPECT_EQ(plan.action, pa::RescueAction::WaitAndRetry);
    EXPECT_TRUE(plan.preserve_context);
    EXPECT_FALSE(plan.record_rejection);
}
