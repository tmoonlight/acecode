#include <gtest/gtest.h>
#include "pa/pa_rescue_driver.hpp"
#include <string>
#include <vector>

namespace {
class RecordingHost final : public acecode::pa::PaRescueHost {
public:
    std::vector<std::string> calls;
    std::vector<int> rejected_tokens;
    bool allow_wait = true;
    bool repair_succeeds = true;
    int withheld_images = 1;
    acecode::pa::CompactOutcome compact_outcome = acecode::pa::CompactOutcome::Compacted;
    int history_tokens() const override { return 10000; }
    void note_rejection(int tokens) override {
        calls.push_back("reject");
        rejected_tokens.push_back(tokens);
    }
    acecode::pa::CompactOutcome compact() override {
        calls.push_back("compact");
        return compact_outcome;
    }
    acecode::ThreadRepairResult repair(const acecode::ThreadRepairOptions& options) override {
        calls.push_back("repair");
        EXPECT_TRUE(options.force_prune_one_group);
        EXPECT_TRUE(options.clear_tool_outputs);
        EXPECT_EQ(options.keep_recent_tool_outputs, 1);
        acecode::ThreadRepairResult result;
        result.status = repair_succeeds ? acecode::ThreadRepairStatus::Repaired
                                       : acecode::ThreadRepairStatus::HistoryExhausted;
        return result;
    }
    int withhold_inline_images() override {
        calls.push_back("withhold");
        return withheld_images;
    }
    bool wait(int) override { calls.push_back("wait"); return allow_wait; }
    void reset_stream() override { calls.push_back("reset"); }
    void history_repaired() override { calls.push_back("repaired"); }
    void notice(const std::string&, nlohmann::json) override { calls.push_back("notice"); }
    void progress(nlohmann::json) override { calls.push_back("progress"); }
    void retry(const acecode::ProviderErrorInfo&, bool waiting, std::string, std::string) override {
        calls.push_back(waiting ? "waiting" : "resuming");
    }
};
}

TEST(PaRescueDriver, CancellationDoesNotResumeOrResetTheStream) {
    // 等待期间取消:已有等待通知保留,不发送恢复事件,也不清掉已显示输出。
    RecordingHost host;
    host.allow_wait = false;
    acecode::pa::RescueState state;
    bool emergency = false;
    EXPECT_FALSE(acecode::pa::run_rescue(host, state, {}, 12000, emergency));
    EXPECT_EQ(host.calls, (std::vector<std::string>{"notice", "waiting", "wait"}));
    EXPECT_EQ(state.same_request_retries, 0);
    EXPECT_EQ(state.wait_retries, 1);
    EXPECT_FALSE(emergency);
}

// 触发场景:拒收已确认(原样重发用完),第一次调用先压缩并重发;压缩后的请求
// (50000)仍被拒,之后再缩一档。
// 期望行为:压缩那次不记账;压缩后仍被拒时才记账一次,规模是压缩前确认拒收的
// 60000;再往后的收缩不再记账。发布顺序:修复 → 清流式草稿 → 进度 → 通知。
TEST(PaRescueDriver, CompactsFirstThenChargesThePreCompactionSizeOnce) {
    RecordingHost host;
    acecode::pa::RescueState state;
    state.active = true;
    state.same_request_retries = acecode::pa::PA_RESCUE_SAME_REQUEST_RETRIES;
    bool emergency = false;
    ASSERT_TRUE(acecode::pa::run_rescue(host, state, {}, 60000, emergency));
    EXPECT_EQ(host.calls, (std::vector<std::string>{"compact", "reset", "progress"}));
    EXPECT_TRUE(host.rejected_tokens.empty()) << "压缩那一步不能下调压缩阈值";
    host.calls.clear();
    ASSERT_TRUE(acecode::pa::run_rescue(host, state, {}, 50000, emergency));
    EXPECT_EQ(host.calls, (std::vector<std::string>{
        "reject", "repair", "repaired", "reset", "progress", "notice"}));
    EXPECT_EQ(host.rejected_tokens, (std::vector<int>{60000}));
    host.calls.clear();
    ASSERT_TRUE(acecode::pa::run_rescue(host, state, {}, 42500, emergency));
    EXPECT_EQ(host.calls, (std::vector<std::string>{
        "repair", "repaired", "reset", "progress", "notice"}));
    EXPECT_EQ(state.shrink_rounds, 2);
}

// 触发场景:压缩没做成(摘要和机械兜底都没腾出空间)。
// 期望行为:同一次调用里立刻收缩并记账,不白白重发同一个大请求。
TEST(PaRescueDriver, FailedCompactionShrinksAndChargesWithinTheSameInvocation) {
    RecordingHost host;
    host.compact_outcome = acecode::pa::CompactOutcome::Failed;
    acecode::pa::RescueState state;
    state.active = true;
    state.same_request_retries = acecode::pa::PA_RESCUE_SAME_REQUEST_RETRIES;
    bool emergency = false;
    ASSERT_TRUE(acecode::pa::run_rescue(host, state, {}, 60000, emergency));
    EXPECT_EQ(host.calls, (std::vector<std::string>{
        "compact", "reject", "repair", "repaired", "reset", "progress", "notice"}));
    EXPECT_EQ(host.rejected_tokens, (std::vector<int>{60000}));
}

// 触发场景:用户在压缩期间点了停止。
// 期望行为:不再收缩、不记账、不清流式草稿,直接返回 false 交给调用方按取消收尾。
TEST(PaRescueDriver, CancelledCompactionStopsWithoutShrinkingOrCharging) {
    RecordingHost host;
    host.compact_outcome = acecode::pa::CompactOutcome::Cancelled;
    acecode::pa::RescueState state;
    state.active = true;
    state.same_request_retries = acecode::pa::PA_RESCUE_SAME_REQUEST_RETRIES;
    bool emergency = false;
    EXPECT_FALSE(acecode::pa::run_rescue(host, state, {}, 60000, emergency));
    EXPECT_EQ(host.calls, (std::vector<std::string>{"compact"}));
    EXPECT_TRUE(host.rejected_tokens.empty());
}

TEST(PaRescueDriver, ExhaustedHistoryAdvancesToEmergencyWithinTheSameInvocation) {
    // 收缩未腾出空间时立刻切紧急档,不能先重发相同的大请求。
    RecordingHost host;
    host.repair_succeeds = false;
    acecode::pa::RescueState state;
    state.active = true;
    state.same_request_retries = acecode::pa::PA_RESCUE_SAME_REQUEST_RETRIES;
    state.compaction_attempted = true;
    bool emergency = false;
    EXPECT_TRUE(acecode::pa::run_rescue(host, state, {}, 60000, emergency));
    EXPECT_TRUE(emergency);
    EXPECT_TRUE(state.shrink_exhausted);
    EXPECT_EQ(host.calls, (std::vector<std::string>{
        "reject", "repair", "reset", "progress", "notice"}));
}

// 触发场景(反馈 LINDANDAN069 原样):估算 36885 tokens、带内联截图的请求被拒。
// 期望行为:先撤图片、立刻重发 —— 不等待、不发「保留完整请求」通知、不记账;
// 发布顺序与收缩一致(撤图 → 清流式草稿 → 进度)。
TEST(PaRescueDriver, InlineImagesAreWithheldBeforeAnyWait) {
    RecordingHost host;
    acecode::pa::RescueState state;
    bool emergency = false;
    ASSERT_TRUE(acecode::pa::run_rescue(host, state, {}, 36885, emergency, true));
    EXPECT_EQ(host.calls, (std::vector<std::string>{"withhold", "reset", "progress"}));
    EXPECT_TRUE(state.images_withheld);
    EXPECT_EQ(state.wait_retries, 0);
    EXPECT_FALSE(emergency);
}

// 触发场景:请求被判带图,但撤图一张也没撤下来(历史里没有图片部件或落盘失败)。
// 期望行为:同一次调用里立刻换下一招(这里是大请求的原样重发),不白白重发;
// 图片可能还在请求里,所以后续压缩、收缩都不记账。
TEST(PaRescueDriver, FailedWithholdFallsThroughWithoutChargingTheLearner) {
    RecordingHost host;
    host.withheld_images = 0;
    acecode::pa::RescueState state;
    bool emergency = false;
    ASSERT_TRUE(acecode::pa::run_rescue(host, state, {}, 60000, emergency, true));
    EXPECT_EQ(host.calls, (std::vector<std::string>{
        "withhold", "notice", "waiting", "wait", "resuming", "reset"}));
    host.calls.clear();
    state.same_request_retries = acecode::pa::PA_RESCUE_SAME_REQUEST_RETRIES;
    ASSERT_TRUE(acecode::pa::run_rescue(host, state, {}, 60000, emergency, true));
    EXPECT_EQ(host.calls, (std::vector<std::string>{"compact", "reset", "progress"}));
    host.calls.clear();
    ASSERT_TRUE(acecode::pa::run_rescue(host, state, {}, 60000, emergency, true));
    EXPECT_EQ(host.calls, (std::vector<std::string>{
        "repair", "repaired", "reset", "progress", "notice"}))
        << "带图请求的收缩不能先 reject 记账";
    EXPECT_TRUE(host.rejected_tokens.empty());
}
