// P0-11 表征测试(split-agent-loop):第 8 组 computer-use 租约释放边界,以及
// 750ms 进度帧 / 500ms 工具输出帧两处节流的精确阈值。
//
// 这两组用例依赖 AgentLoop 的两个测试注入点(set_progress_clock_for_tests /
// set_computer_use_release_for_tests):默认路径与原实现逐字相同,这里注入的
// 假时钟只按用例要求推进,不依赖 sleep;假租约只记录 owner 与释放次数,不启动
// 真实桌面 helper。所有断言钉住的是当前行为,不修 bug。
#include "test_support/agent_loop/characterization_fixture.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace acecode;
using namespace acecode_test::characterization;
using Clock = std::chrono::steady_clock;

// 可控单调时钟:各线程通过 atomic 读同一份毫秒计数;起点取非零值,避免与
// 「last_emit_at 尚未初始化」的 0 混淆。
struct FakeClock {
    std::atomic<std::int64_t> now_ms{1'000'000};
    Clock::time_point now() const {
        return Clock::time_point(std::chrono::milliseconds(now_ms.load()));
    }
    void advance(std::int64_t ms) { now_ms.fetch_add(ms); }
};

struct ClockStep {
    std::int64_t advance_ms;
    StreamEvent event;
};

// 在两个流事件之间推进假时钟的 provider:节流判定发生在每个 delta 的处理里,
// 只有 provider 自己能把「时钟推进」精确插在两个 delta 之间。
class ClockSteppingProvider : public acecode_test::StubLlmProvider {
public:
    ClockSteppingProvider(std::shared_ptr<FakeClock> clock, std::vector<ClockStep> steps)
        : clock_(std::move(clock)), steps_(std::move(steps)) {}

    void chat_stream(const std::vector<ChatMessage>&, const std::vector<ToolDef>&,
                     const StreamCallback& callback, std::atomic<bool>* = nullptr) override {
        std::vector<ClockStep> steps;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            steps.swap(steps_);
        }
        for (const auto& step : steps) {
            clock_->advance(step.advance_ms);
            callback(step.event);
        }
        StreamEvent done;
        done.type = StreamEventType::Done;
        done.finish_reason = "stop";
        callback(done);
    }

private:
    std::mutex mutex_;
    std::shared_ptr<FakeClock> clock_;
    std::vector<ClockStep> steps_;
};

StreamEvent reasoning_delta(std::string text) {
    StreamEvent event;
    event.type = StreamEventType::ReasoningDelta;
    event.content = std::move(text);
    return event;
}

StreamEvent text_delta(std::string text) {
    StreamEvent event;
    event.type = StreamEventType::Delta;
    event.content = std::move(text);
    return event;
}

// 租约 / 回合收尾 / Done 三类事件的共同轨迹;fake 释放函数、after_turn 回调与
// 事件订阅者分别在各自线程上追加,顺序即真实发生顺序。
struct LeaseTrace {
    std::mutex mutex;
    std::vector<std::string> entries;
    void push(std::string entry) {
        std::lock_guard<std::mutex> lock(mutex);
        entries.push_back(std::move(entry));
    }
    std::vector<std::string> snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        return entries;
    }
    int releases() {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<int>(std::count_if(entries.begin(), entries.end(),
            [](const std::string& entry) { return entry.rfind("release:", 0) == 0; }));
    }
};

struct LeaseProbe {
    explicit LeaseProbe(Harness& h) : harness(h), trace(std::make_shared<LeaseTrace>()) {
        harness.loop->set_computer_use_release_for_tests(
            [trace = trace](const std::string& session_id) { trace->push("release:" + session_id); });
        {
            std::lock_guard<std::mutex> lock(harness.observed->mutex);
            harness.observed->after_turn = [trace = trace] { trace->push("turn_finished"); };
        }
        subscription = harness.loop->events().subscribe([trace = trace](const SessionEvent& event) {
            if (event.kind == SessionEventKind::Done) trace->push("done");
        });
    }
    ~LeaseProbe() { harness.loop->events().unsubscribe(subscription); }
    Harness& harness;
    std::shared_ptr<LeaseTrace> trace;
    EventDispatcher::SubscriptionId subscription = 0;
};

std::vector<SessionEvent> progress_frames(const Observation& observed, const std::string& phase) {
    std::vector<SessionEvent> found;
    for (const auto& event : events_of(observed, SessionEventKind::AgentProgress)) {
        if (event.payload.value("phase", std::string{}) == phase) found.push_back(event);
    }
    return found;
}

// 场景:普通文本回合正常收尾。期望:显式释放先于 on_turn_finished 与 Done,
// 函数返回时 RAII 再释放一次(同一 owner);回归会让桌面租约在回合结束后仍被占住,
// 下一回合或另一会话拿不到 computer-use。
TEST(ComputerUseLeaseGolden, NormalTurnReleasesBeforeTurnFinishedAndAgainOnScopeExit) {
    Isolation isolation;
    Harness h(isolation);
    LeaseProbe probe(h);
    h.provider->push_text("finished");
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("plain turn"); }));
    const std::string owner = "release:" + h.session->current_session_id();
    const std::vector<std::string> expected{owner, "turn_finished", "done", owner};
    EXPECT_EQ(probe.trace->snapshot(), expected);
}

// 场景:UserPromptSubmit hook 拦截了提示词,回合早退。期望:早退路径没有显式释放,
// 只剩 RAII 在 Done 之后释放一次;回归会在早退路径上漏掉释放(租约泄漏)或提前
// 释放两次。
TEST(ComputerUseLeaseGolden, HookBlockedPromptReleasesOnlyThroughRaii) {
    Isolation isolation;
    Harness h(isolation);
    LeaseProbe probe(h);
    h.install_hooks({"UserPromptSubmit"}, [](const Json& payload) {
        if (payload["hook_event_name"] == "UserPromptSubmit") {
            return Json{{"decision", "block"}, {"reason", "blocked for the lease probe"}};
        }
        return Json::object();
    });
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("blocked turn"); }));
    const std::string owner = "release:" + h.session->current_session_id();
    const std::vector<std::string> expected{"turn_finished", "done", owner};
    EXPECT_EQ(probe.trace->snapshot(), expected);
    EXPECT_EQ(h.provider->turn_count(), 0);
}

// 场景:模型仍在响应时用户 abort。期望:abort() 立即释放一次(不等回合结束),
// 收尾再显式释放一次,RAII 最后一次,三次 owner 相同;回归会让 abort 之后桌面
// helper 继续被本会话占用直到回合真正结束。
TEST(ComputerUseLeaseGolden, AbortReleasesImmediatelyThenAgainAtTurnEnd) {
    Isolation isolation;
    Harness h(isolation);
    LeaseProbe probe(h);
    h.provider->set_latency_ms(3000);
    h.provider->push_text("late reply");
    ASSERT_TRUE(h.perform([&h] {
        h.loop->submit("abort me");
        const auto deadline = Clock::now() + 2s;
        while (!h.loop->is_busy() && Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_TRUE(h.loop->is_busy());
        h.loop->abort();
    }));
    const std::string owner = "release:" + h.session->current_session_id();
    const auto trace = probe.trace->snapshot();
    ASSERT_FALSE(trace.empty());
    EXPECT_EQ(trace.front(), owner) << "abort() must release before anything else";
    EXPECT_EQ(trace.back(), owner) << "RAII releases last";
    EXPECT_EQ(probe.trace->releases(), 3) << "abort + turn end + RAII";
    const auto finished = std::find(trace.begin(), trace.end(), "turn_finished");
    ASSERT_NE(finished, trace.end());
    EXPECT_EQ(std::count(trace.begin(), finished, owner), 2)
        << "both the abort release and the turn-end release precede on_turn_finished";
}

// 场景:工具执行抛出 C++ 异常。期望:异常被工具运行器吞成失败结果,回合照常
// 收尾,释放序列与普通回合完全相同;回归会让异常越过收尾把释放留给 RAII,或者
// 让回合根本不结束。
TEST(ComputerUseLeaseGolden, ToolExceptionStillReleasesExactlyLikeANormalTurn) {
    Isolation isolation;
    Harness h(isolation);
    LeaseProbe probe(h);
    auto tool = h.probe("throwing_probe", true);
    tool.execute = [](const std::string&, const ToolContext&) -> ToolResult {
        throw std::runtime_error("probe exploded");
    };
    h.tools.register_tool(std::move(tool));
    ASSERT_TRUE(h.run_calls({{"boom", "throwing_probe", "{}"}}));
    const std::string owner = "release:" + h.session->current_session_id();
    const std::vector<std::string> expected{owner, "turn_finished", "done", owner};
    EXPECT_EQ(probe.trace->snapshot(), expected);
    std::lock_guard<std::mutex> lock(h.observed->mutex);
    ASSERT_FALSE(h.observed->results.empty());
    EXPECT_FALSE(h.observed->results.front().success);
}

// 场景:同一 key(reasoning)的进度帧连续到达,相邻两帧相隔 749ms 与 750ms。
// 期望:key 变化的首帧强制发送;之后 749ms 被合并、恰到 750ms 发送 —— 阈值是
// `elapsed < 750ms` 时丢弃;回归会把阈值改成 <= 或 > ,或者把 750ms 常量改掉。
TEST(ProgressThrottleGolden, ReasoningFramesRespectThe750msWindowExactly) {
    Isolation isolation;
    auto clock = std::make_shared<FakeClock>();
    auto provider = std::make_shared<ClockSteppingProvider>(clock, std::vector<ClockStep>{
        {0, reasoning_delta("r1")},
        {749, reasoning_delta("r2")},   // 749ms:合并
        {1, reasoning_delta("r3")},     // 恰好 750ms:发送
        {749, reasoning_delta("r4")},   // 合并
        {1, reasoning_delta("r5")},     // 发送
        {0, text_delta("ok")},
    });
    Harness h(isolation, "clock", provider);
    h.loop->set_progress_clock_for_tests([clock] { return clock->now(); });
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("think"); }));
    std::lock_guard<std::mutex> lock(h.observed->mutex);
    const auto frames = progress_frames(*h.observed, "reasoning");
    ASSERT_EQ(frames.size(), 3u);
    for (const auto& frame : frames) EXPECT_EQ(frame.payload.value("label", std::string{}), "正在推理");
    EXPECT_EQ(events_of(*h.observed, SessionEventKind::Reasoning).size(), 5u)
        << "throttling only affects progress frames; every reasoning chunk is still published";
}

// 场景:时钟完全不推进。期望:五个 reasoning delta 只产生一帧进度(首帧强制),
// 其余全部合并;回归会让节流失效,每个 delta 都刷一帧。
TEST(ProgressThrottleGolden, FrozenClockCoalescesEveryLaterReasoningFrame) {
    Isolation isolation;
    auto clock = std::make_shared<FakeClock>();
    auto provider = std::make_shared<ClockSteppingProvider>(clock, std::vector<ClockStep>{
        {0, reasoning_delta("r1")}, {0, reasoning_delta("r2")}, {0, reasoning_delta("r3")},
        {0, reasoning_delta("r4")}, {0, reasoning_delta("r5")}, {0, text_delta("ok")},
    });
    Harness h(isolation, "frozen", provider);
    h.loop->set_progress_clock_for_tests([clock] { return clock->now(); });
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("think"); }));
    std::lock_guard<std::mutex> lock(h.observed->mutex);
    EXPECT_EQ(progress_frames(*h.observed, "reasoning").size(), 1u);
}

// 场景:工具在 500ms 内连续输出多段。期望:首段立即发 tool_update,之后 499ms
// 合并、恰到 500ms 发送(阈值是 `>= 500ms` 发送);回归会改掉 500ms 常量或比较符,
// 让 Web/TUI 的实时输出要么刷屏要么冻结。
TEST(ToolStreamThrottleGolden, ToolUpdateFramesRespectThe500msWindowExactly) {
    Isolation isolation;
    Harness h(isolation);
    auto clock = std::make_shared<FakeClock>();
    h.loop->set_progress_clock_for_tests([clock] { return clock->now(); });
    auto tool = h.probe("stream_probe", true);
    tool.execute = [clock](const std::string&, const ToolContext& context) {
        if (!context.stream) return ToolResult{"no stream channel", false};
        context.stream("line 1\n");
        clock->advance(499);
        context.stream("line 2\n");   // 499ms:合并
        clock->advance(1);
        context.stream("line 3\n");   // 500ms:发送
        clock->advance(499);
        context.stream("line 4\n");   // 合并
        clock->advance(1);
        context.stream("line 5\n");   // 发送
        return ToolResult{"streamed", true};
    };
    h.tools.register_tool(std::move(tool));
    ASSERT_TRUE(h.run_calls({{"stream-1", "stream_probe", "{}"}}));
    std::lock_guard<std::mutex> lock(h.observed->mutex);
    ASSERT_FALSE(h.observed->results.empty());
    ASSERT_TRUE(h.observed->results.front().success) << h.observed->results.front().output;
    const auto updates = events_of(*h.observed, SessionEventKind::ToolUpdate);
    ASSERT_EQ(updates.size(), 3u);
    EXPECT_EQ(updates.back().payload.value("total_lines", 0), 5);
    for (const auto& update : updates) {
        EXPECT_EQ(update.payload.value("tool_call_id", std::string{}), "stream-1");
    }
}
} // namespace
