// 自动模式（试验）的两阶段裁决(openspec add-auto-review-mode,规格「两阶段裁决」
// 「回答的解析」「时限与中断」)。这里直接测裁决函数本身,不经过 AgentLoop:
// 关心的是「问几次、每次问什么、各种回答与失败分别归成哪种结果」。

#include <gtest/gtest.h>

#include "agent/approval/auto_review_gate.hpp"
#include "permissions/auto_review_prompt.hpp"

#include "test_support/agent/scripted_reviewer.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using acecode::agent::ReviewOutcome;
using acecode::agent::ReviewTimeouts;
using acecode::agent::run_two_stage_review;
using acecode_test::ScriptedReviewer;
using Kind = acecode::agent::ReviewOutcome::Kind;

namespace {

const std::string kTranscript = "User: run the tests\n";
const std::string kAction = "Tool bash {\"command\":\"pnpm test\"}\nExecution: with full access (no sandbox)\n";

ReviewOutcome review(const std::shared_ptr<ScriptedReviewer>& reviewer,
                     const std::atomic<bool>& abort, ReviewTimeouts timeouts = {}) {
    return run_two_stage_review(reviewer, kTranscript, kAction, "session-1:auto-review", abort, timeouts);
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

// 场景:第一阶段就回答「不拦」。
// 期望:直接放行,只发出一次请求,阶段记为 review_stage1(审计里据此区分)。
TEST(AutoReviewGate, FirstStageAllowEndsTheReview) {
    auto reviewer = std::make_shared<ScriptedReviewer>();
    reviewer->push("<block>no</block>");
    std::atomic<bool> abort{false};

    const auto outcome = review(reviewer, abort);
    EXPECT_EQ(outcome.kind, Kind::Allowed);
    EXPECT_EQ(outcome.stage, "review_stage1");
    EXPECT_EQ(outcome.model, "reviewer-1");
    EXPECT_EQ(reviewer->calls(), 1);
}

// 场景:第一阶段宁可错拦地答了「拦」,第二阶段结合例外与用户意图后改判「不拦」。
// 期望:以第二阶段为准放行,共两次请求。这是两阶段设计的意义:第一阶段的误拦
// 由第二阶段纠正,而大多数调用只花一次请求。
TEST(AutoReviewGate, SecondStageCanOverturnTheFirst) {
    auto reviewer = std::make_shared<ScriptedReviewer>();
    reviewer->push("<block>yes</block>");
    reviewer->push("<think>the user asked for it</think><block>no</block>");
    std::atomic<bool> abort{false};

    const auto outcome = review(reviewer, abort);
    EXPECT_EQ(outcome.kind, Kind::Allowed);
    EXPECT_EQ(outcome.stage, "review_stage2");
    EXPECT_EQ(reviewer->calls(), 2);
}

// 场景:两个阶段都判「拦」,第二阶段给了理由。
// 期望:结果是拦下,理由原样带出(之后回给主模型并写进审计)。
TEST(AutoReviewGate, SecondStageBlockCarriesTheReason) {
    auto reviewer = std::make_shared<ScriptedReviewer>();
    reviewer->push("<block>yes</block>");
    reviewer->push("<block>yes</block><reason>B2: force-push rewrites published history</reason>");
    std::atomic<bool> abort{false};

    const auto outcome = review(reviewer, abort);
    EXPECT_EQ(outcome.kind, Kind::Blocked);
    EXPECT_EQ(outcome.reason, "B2: force-push rewrites published history");
    EXPECT_EQ(outcome.stage, "review_stage2");
}

// 场景:第一阶段没有按格式回答(弱模型常见)。
// 期望:不当作结论,进入第二阶段再问一次;第二阶段答「不拦」则放行。
TEST(AutoReviewGate, UnreadableFirstStageEscalatesToTheSecond) {
    auto reviewer = std::make_shared<ScriptedReviewer>();
    reviewer->push("Sure, this looks fine to me.");
    reviewer->push("<block>no</block>");
    std::atomic<bool> abort{false};

    const auto outcome = review(reviewer, abort);
    EXPECT_EQ(outcome.kind, Kind::Allowed);
    EXPECT_EQ(reviewer->calls(), 2);
}

// 场景:第二阶段的回答读不出结论 —— 没有标记,或者 yes / no 都出现。
// 期望:结果是「没有结果」,不是拦下。回归意义:记成拦下的话,弱模型的每次格式
// 错误都会计入拦截次数,几次之后回合就被锁死。
TEST(AutoReviewGate, UnreadableSecondStageIsNoVerdictNotABlock) {
    for (const char* answer : {"I am not sure about this one.",
                               "<block>yes</block> or maybe <block>no</block>"}) {
        auto reviewer = std::make_shared<ScriptedReviewer>();
        reviewer->push("<block>yes</block>");
        reviewer->push(answer);
        std::atomic<bool> abort{false};

        const auto outcome = review(reviewer, abort);
        EXPECT_EQ(outcome.kind, Kind::NoVerdict) << answer;
        EXPECT_EQ(outcome.stage, "review_stage2");
        EXPECT_FALSE(outcome.reason.empty());
    }
}

// 场景:裁决服务返回 HTTP 429(限流)。
// 期望:「没有结果」,原因里带状态码;第一阶段就失败时不再发第二次请求
// (服务够不着时再问一遍只会多耗一次额度)。
TEST(AutoReviewGate, RequestFailureIsNoVerdictAndDoesNotRetry) {
    auto reviewer = std::make_shared<ScriptedReviewer>();
    reviewer->push_error(429);
    std::atomic<bool> abort{false};

    const auto outcome = review(reviewer, abort);
    EXPECT_EQ(outcome.kind, Kind::NoVerdict);
    EXPECT_EQ(outcome.reason, "HTTP 429");
    EXPECT_EQ(outcome.stage, "review_stage1");
    EXPECT_EQ(reviewer->calls(), 1);
}

// 场景:第一阶段的请求迟迟不返回(这里 3 秒),时限设成 80 毫秒。
// 期望:到时就返回「没有结果」(原因 timeout),不会等到请求自己结束;整个调用
// 远小于 3 秒。80 毫秒只是为了让单测快,生产默认是 20 秒 / 45 秒。
TEST(AutoReviewGate, StageTimeoutIsNoVerdict) {
    auto reviewer = std::make_shared<ScriptedReviewer>();
    reviewer->push_slow(3000, "<block>no</block>");
    std::atomic<bool> abort{false};

    const auto started = std::chrono::steady_clock::now();
    const auto outcome = review(reviewer, abort,
        ReviewTimeouts{std::chrono::milliseconds(80), std::chrono::milliseconds(80)});
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(outcome.kind, Kind::NoVerdict);
    EXPECT_EQ(outcome.reason, "timeout");
    EXPECT_LT(elapsed, std::chrono::milliseconds(1500));
}

// 场景:裁决请求进行中,用户中断了回合。
// 期望:立即返回「已中断」,被裁决的调用不会执行;不会等请求自己结束。
TEST(AutoReviewGate, UserAbortCancelsTheReview) {
    auto reviewer = std::make_shared<ScriptedReviewer>();
    reviewer->push_slow(3000, "<block>no</block>");
    std::atomic<bool> abort{false};
    std::thread interrupter([&abort] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        abort.store(true);
    });

    const auto started = std::chrono::steady_clock::now();
    const auto outcome = review(reviewer, abort);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    interrupter.join();

    EXPECT_EQ(outcome.kind, Kind::Aborted);
    EXPECT_LT(elapsed, std::chrono::milliseconds(1500));
}

// 场景:没有可用的模型,或者选中的模型不支持不带工具的对话(自带工具运行时的
// provider)。
// 期望:都是「没有结果」,并且根本不发请求。
TEST(AutoReviewGate, UnusableProviderIsNoVerdict) {
    std::atomic<bool> abort{false};
    const auto missing = run_two_stage_review(nullptr, kTranscript, kAction, "", abort);
    EXPECT_EQ(missing.kind, Kind::NoVerdict);
    EXPECT_FALSE(missing.reason.empty());

    auto reviewer = std::make_shared<ScriptedReviewer>();
    reviewer->set_tool_free_chat(false);
    const auto outcome = review(reviewer, abort);
    EXPECT_EQ(outcome.kind, Kind::NoVerdict);
    EXPECT_EQ(reviewer->calls(), 0);
}

// 场景:检查实际发给裁决模型的请求。
// 期望:
//   - 不带任何工具定义,也不带压缩标记(裁决不走摘要压缩那条路,失败不能被
//     当成上下文超限去学习);
//   - 缓存键是会话 id 加固定后缀,与主对话分开;
//   - system 是常量提示,user 里依次是记录、待裁决的调用、阶段指令;
//   - 两个阶段只有末尾的指令不同。
TEST(AutoReviewGate, RequestShapeIsToolFreeAndNotACompactionCall) {
    auto reviewer = std::make_shared<ScriptedReviewer>();
    reviewer->push("<block>yes</block>");
    reviewer->push("<block>no</block>");
    std::atomic<bool> abort{false};
    ASSERT_EQ(review(reviewer, abort).kind, Kind::Allowed);

    const auto requests = reviewer->requests();
    ASSERT_EQ(requests.size(), 2u);
    for (const auto& request : requests) {
        EXPECT_EQ(request.tool_count, 0u);
        EXPECT_FALSE(request.for_compaction);
        EXPECT_EQ(request.prompt_cache_key, "session-1:auto-review");
        ASSERT_EQ(request.messages.size(), 2u);
        EXPECT_EQ(request.messages[0].role, "system");
        EXPECT_EQ(request.messages[0].content, acecode::auto_review::reviewer_system_prompt());
    }
    const std::string quick = reviewer->user_text(0);
    const std::string full = reviewer->user_text(1);
    EXPECT_TRUE(contains(quick, "<transcript>\nUser: run the tests\n</transcript>"));
    EXPECT_TRUE(contains(quick, "<action>\nTool bash {\"command\":\"pnpm test\"}"));
    EXPECT_TRUE(contains(quick, acecode::auto_review::stage_instruction(acecode::auto_review::ReviewStage::Quick)));
    EXPECT_TRUE(contains(full, acecode::auto_review::stage_instruction(acecode::auto_review::ReviewStage::Full)));
    EXPECT_FALSE(contains(quick, "<reason>"));
}
