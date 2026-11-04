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
// 期望行为:第三步先做摘要压缩,不收缩、不给学习器记账;记下这次确认拒收的规模,
// 留给「压缩也没解决」时记账用。
// 回归背景(用户 2026-11-04):旧实现这一步直接收缩并记账,一报「上下文过大」
// 压缩阈值就下调,而那次拒收本来一次压缩就能绕过去。
TEST(PaOverflowRescue, CompactsOnceAfterRetriesFailWithoutRecording) {
    pa::RescueState state;
    step(state, inputs(100000, 60000));
    step(state, inputs(100000, 60000));

    const auto plan = step(state, inputs(100000, 60000));
    EXPECT_EQ(plan.action, pa::RescueAction::CompactHistory);
    EXPECT_EQ(plan.wait_ms, 0);
    EXPECT_FALSE(plan.record_rejection);
    EXPECT_FALSE(state.rejection_recorded);
    EXPECT_TRUE(state.compaction_attempted);
    EXPECT_EQ(state.confirmed_rejected_tokens, 100000);
    EXPECT_EQ(state.shrink_rounds, 0);
}

// 触发场景:确认拒收后压缩过一次,压缩后的请求(70000,历史 30000)仍被拒。
// 期望行为:这时才收缩,目标 = 被拒请求的 85% 再扣掉历史之外的固定部分
// (70000 × 85% = 59500,固定 40000,历史目标 19500);记一次账,规模按压缩前
// 那次确认拒收的 100000 —— 压缩后的规模小得多,拿它当上限会把窗口砍过头。
// 不再原样重发、也不再压缩第二次。
TEST(PaOverflowRescue, ShrinksAndRecordsTheConfirmedSizeWhenCompactionDidNotHelp) {
    pa::RescueState state;
    step(state, inputs(100000, 60000));
    step(state, inputs(100000, 60000));
    step(state, inputs(100000, 60000));

    const auto plan = step(state, inputs(70000, 30000));
    EXPECT_EQ(plan.action, pa::RescueAction::ShrinkHistory);
    EXPECT_EQ(plan.target_history_tokens, 19500);
    EXPECT_TRUE(plan.record_rejection);
    EXPECT_EQ(plan.rejected_tokens, 100000);
    EXPECT_TRUE(state.rejection_recorded);
    EXPECT_EQ(state.shrink_rounds, 1);
    EXPECT_EQ(state.same_request_retries, 2);
}

// 触发场景:调用方压缩失败(摘要与机械兜底都没腾出空间),同一次调用里换下一招,
// 被拒请求还是原来那个。
// 期望行为:不再提议压缩,直接按 85% 收缩并记账,规模就是这次确认拒收的规模。
TEST(PaOverflowRescue, FailedCompactionFallsThroughToShrinkAndRecord) {
    pa::RescueState state;
    step(state, inputs(100000, 60000));
    step(state, inputs(100000, 60000));
    ASSERT_EQ(step(state, inputs(100000, 60000)).action,
              pa::RescueAction::CompactHistory);

    const auto plan = step(state, inputs(100000, 60000));
    EXPECT_EQ(plan.action, pa::RescueAction::ShrinkHistory);
    EXPECT_EQ(plan.target_history_tokens, 45000);
    EXPECT_TRUE(plan.record_rejection);
    EXPECT_EQ(plan.rejected_tokens, 100000);
}

// 触发场景:收缩过一档之后重发仍被拒。
// 期望行为:继续收缩下一档(85000 × 85% = 72250,固定 40000 → 32250),不再
// 原样重发,也不再记账 —— 一个 episode 只记最初那次确认拒收的规模,否则一串
// 越来越小的被拒规模会把压缩窗口一路砍到远小于真实上限。
TEST(PaOverflowRescue, KeepsShrinkingWithoutFurtherRetriesOrRecording) {
    pa::RescueState state;
    step(state, inputs(100000, 60000));
    step(state, inputs(100000, 60000));
    step(state, inputs(100000, 60000));  // 压缩
    step(state, inputs(100000, 60000));  // 第一档收缩

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
    ASSERT_EQ(step(state, inputs(60000, 2000)).action,
              pa::RescueAction::CompactHistory);
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
    state.compaction_attempted = true;
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
// 期望行为:紧急档被拒不是抽风该有的样子(紧急档已经很小),跳过原样重发,
// 先压缩;压缩后仍被拒才收缩。紧急档规模始终不记账。
TEST(PaOverflowRescue, EmergencyRejectionSkipsSameRequestRetriesAndRecording) {
    pa::RescueState state;

    const auto compact = step(state, inputs(40000, 30000, /*emergency=*/true));
    EXPECT_EQ(compact.action, pa::RescueAction::CompactHistory);
    EXPECT_FALSE(compact.record_rejection);
    EXPECT_EQ(state.same_request_retries, 0);

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
    state.compaction_attempted = true;

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

// 触发场景:不足 40000 tokens 的请求被反复拒收。
// 期望行为:保留完整请求(不收缩、不切紧急档、不给学习器记账),按 5、10、20、
// 40 秒后保持 60 秒退避,最多 PA_RESCUE_MAX_WAIT_RETRIES 次;耗尽后放弃,放弃的
// plan 仍带 preserve_context,调用方据此给出「历史没动过」的文案。计数饱和时同样放弃。
// 回归背景(反馈 LINDANDAN069):曾经无限次等待 —— 拒收真正的原因(一张以 base64
// 发出去的截图)不在估算里,同一个请求原样重发了 830 多次、空等 14 小时。
TEST(PaOverflowRescue, SmallRequestsWaitUpToTheCapThenGiveUpWithoutShrinkingOrLearning) {
    pa::RescueState state;
    const int delays[] = {5000, 10000, 20000, 40000};
    for (int i = 0; i < pa::PA_RESCUE_MAX_WAIT_RETRIES; ++i) {
        const auto plan = step(state, inputs(39999, 30000));
        EXPECT_EQ(plan.action, pa::RescueAction::WaitAndRetry);
        EXPECT_TRUE(plan.preserve_context);
        EXPECT_FALSE(plan.record_rejection);
        EXPECT_EQ(plan.target_history_tokens, 0);
        EXPECT_EQ(plan.wait_ms, i < 4 ? delays[i] : 60000);
    }
    const auto give_up = step(state, inputs(39999, 30000));
    EXPECT_EQ(give_up.action, pa::RescueAction::GiveUp);
    EXPECT_TRUE(give_up.preserve_context);
    EXPECT_FALSE(give_up.record_rejection);
    EXPECT_EQ(state.shrink_rounds, 0);
    EXPECT_EQ(state.same_request_retries, 0);
    EXPECT_FALSE(state.rejection_recorded);

    pa::RescueState saturated;
    saturated.wait_retries = std::numeric_limits<int>::max();
    EXPECT_EQ(step(saturated, inputs(20000, 10000)).action, pa::RescueAction::GiveUp);
    EXPECT_EQ(saturated.wait_retries, std::numeric_limits<int>::max());
}

// 触发场景(反馈 LINDANDAN069 原样):估算 36885 tokens 的请求带着内联截图被拒。
// 期望行为:第一步撤图片(不等待、不记账),排在小请求等待之前;撤过一次就不再
// 提议,哪怕调用方仍说带图(撤图失败),之后按小请求规则等待。
TEST(PaOverflowRescue, InlineImageRejectionWithholdsImagesBeforeWaiting) {
    pa::RescueState state;
    auto in = inputs(36885, 29252);
    in.inline_images = true;

    const auto first = step(state, in);
    EXPECT_EQ(first.action, pa::RescueAction::WithholdImages);
    EXPECT_EQ(first.wait_ms, 0);
    EXPECT_FALSE(first.record_rejection);
    EXPECT_TRUE(state.images_withheld);
    EXPECT_EQ(state.wait_retries, 0);

    const auto second = step(state, in);
    EXPECT_EQ(second.action, pa::RescueAction::WaitAndRetry);
    EXPECT_TRUE(second.preserve_context);
}

// 触发场景:大请求带着内联图片被拒,撤图没能撤掉(落盘失败),调用方仍报带图。
// 期望行为:继续原样重发 → 压缩 → 收缩,但整个过程都不给学习器记账 —— 带图请求
// 的规模不代表服务端能收多大。
// 回归背景(反馈 LINDANDAN069):一次带图拒收被记成 43407 tokens 的上限,把
// aicoder-pro 的压缩阈值从约 13.5 万砍到 4 万,同进程里别的会话也跟着频繁压缩。
TEST(PaOverflowRescue, InlineImageRejectionNeverChargesTheBudgetLearner) {
    pa::RescueState state;
    auto in = inputs(100000, 60000);
    in.inline_images = true;

    EXPECT_EQ(step(state, in).action, pa::RescueAction::WithholdImages);
    EXPECT_EQ(step(state, in).action, pa::RescueAction::RetrySameRequest);
    EXPECT_EQ(step(state, in).action, pa::RescueAction::RetrySameRequest);
    EXPECT_EQ(step(state, in).action, pa::RescueAction::CompactHistory);
    EXPECT_EQ(state.confirmed_rejected_tokens, 0) << "带图请求的规模不能留作记账依据";
    const auto shrink = step(state, in);
    EXPECT_EQ(shrink.action, pa::RescueAction::ShrinkHistory);
    EXPECT_FALSE(shrink.record_rejection);
    state.shrink_exhausted = true;
    const auto emergency = step(state, in);
    EXPECT_EQ(emergency.action, pa::RescueAction::EmergencyProfile);
    EXPECT_FALSE(emergency.record_rejection);
    EXPECT_FALSE(state.rejection_recorded);
}

// 触发场景:4 万的请求确认拒收,压缩后的请求(3.4 万)仍被拒。
// 期望行为:压缩后低于 4 万下限,转为保留上下文的等待,不收缩、不记账。
TEST(PaOverflowRescue, LargeRecoveryStopsShrinkingOnceRequestFallsBelowFloor) {
    pa::RescueState state;
    state.same_request_retries = pa::PA_RESCUE_SAME_REQUEST_RETRIES;
    EXPECT_EQ(step(state, inputs(40000, 30000)).action, pa::RescueAction::CompactHistory);
    const auto plan = step(state, inputs(34000, 24000));
    EXPECT_EQ(plan.action, pa::RescueAction::WaitAndRetry);
    EXPECT_TRUE(plan.preserve_context);
    EXPECT_FALSE(plan.record_rejection);
    EXPECT_FALSE(state.rejection_recorded);
}
