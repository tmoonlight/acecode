#include <gtest/gtest.h>
#include "pa/pa_rescue_driver.hpp"
#include <string>
#include <vector>

namespace {
class RecordingHost final : public acecode::pa::PaRescueHost {
public:
    std::vector<std::string> calls;
    bool allow_wait = true;
    bool repair_succeeds = true;
    int history_tokens() const override { return 10000; }
    void note_rejection(int) override { calls.push_back("reject"); }
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

TEST(PaRescueDriver, SuccessfulRepairKeepsPublicationOrderAndChargesRejectionOnce) {
    RecordingHost host;
    acecode::pa::RescueState state;
    state.active = true;
    state.same_request_retries = acecode::pa::PA_RESCUE_SAME_REQUEST_RETRIES;
    bool emergency = false;
    ASSERT_TRUE(acecode::pa::run_rescue(host, state, {}, 60000, emergency));
    EXPECT_EQ(host.calls, (std::vector<std::string>{
        "reject", "repair", "repaired", "reset", "progress", "notice"}));
    host.calls.clear();
    ASSERT_TRUE(acecode::pa::run_rescue(host, state, {}, 50000, emergency));
    EXPECT_EQ(host.calls, (std::vector<std::string>{
        "repair", "repaired", "reset", "progress", "notice"}));
    EXPECT_EQ(state.shrink_rounds, 2);
}

TEST(PaRescueDriver, ExhaustedHistoryAdvancesToEmergencyWithinTheSameInvocation) {
    // 收缩未腾出空间时立刻切紧急档,不能先重发相同的大请求。
    RecordingHost host;
    host.repair_succeeds = false;
    acecode::pa::RescueState state;
    state.active = true;
    state.same_request_retries = acecode::pa::PA_RESCUE_SAME_REQUEST_RETRIES;
    bool emergency = false;
    EXPECT_TRUE(acecode::pa::run_rescue(host, state, {}, 60000, emergency));
    EXPECT_TRUE(emergency);
    EXPECT_TRUE(state.shrink_exhausted);
    EXPECT_EQ(host.calls, (std::vector<std::string>{
        "reject", "repair", "reset", "progress", "notice"}));
}
