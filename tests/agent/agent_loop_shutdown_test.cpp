#include <gtest/gtest.h>
#include "test_support/agent/agent_loop_fixture.hpp"
#include "test_support/agent_loop/characterization_fixture.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "utils/scope_exit.hpp"

namespace {
using namespace std::chrono_literals;
using acecode_test::characterization::Isolation;
using acecode_test::characterization::Harness;
}

// 场景：尚未启动时 control 持有最后一个 loop 引用。期望 shutdown 在锁外
// 清队列且不再访问 this，允许最终引用释放并重入析构；此前自持环无法释放。
TEST(AgentLoopShutdown, RetiredControlMayReleaseTheLastOwnerOnCallerThread) {
    Isolation isolation;
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode_test::AgentLoopFixture fixture({}, tools, {}, isolation.directory.path.string(), permissions);
    std::shared_ptr<acecode::AgentLoop> loop = fixture.make();
    std::weak_ptr<acecode::AgentLoop> weak = loop;
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto receipt = loop->enqueue_control([owner = loop, calls] { ++*calls; return true; });
    auto* caller = loop.get();
    loop.reset();
    ASSERT_FALSE(weak.expired());
    caller->shutdown();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(calls->load(), 0);
    EXPECT_FALSE(receipt.completed());
    EXPECT_TRUE(receipt.cancelled());
    EXPECT_FALSE(receipt.wait_for_completion(1s));
}

// 场景：工具运行中排队 control 后退出。期望运行中的工具收到取消并 join，
// 排队控制不执行、等待者不挂死；同时覆盖真实 worker 的退出路径。
TEST(AgentLoopShutdown, RunningTurnCancelsQueuedControlAndReleasesItsCapture) {
    Isolation isolation;
    Harness harness(isolation);
    auto entered = std::make_shared<std::promise<void>>();
    auto ready = entered->get_future();
    auto tool = harness.probe("wait_for_abort", true);
    tool.execute = [entered](const std::string&, const acecode::ToolContext& context) {
        entered->set_value();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (context.abort_flag && !context.abort_flag->load()
            && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
        return acecode::ToolResult{"cancelled", false};
    };
    harness.tools.register_tool(std::move(tool));
    harness.provider->push_tool_call("wait_for_abort", "{}");
    harness.loop->submit("run");
    ASSERT_EQ(ready.wait_for(2s), std::future_status::ready);
    auto capture = std::make_shared<int>(1);
    std::weak_ptr<int> weak = capture;
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto receipt = harness.loop->enqueue_control([capture, calls] { ++*calls; return true; });
    capture.reset();
    ASSERT_TRUE(receipt.queued_behind_turn);
    harness.loop->shutdown();
    EXPECT_EQ(calls->load(), 0);
    EXPECT_TRUE(weak.expired());
    EXPECT_TRUE(receipt.cancelled());
    EXPECT_FALSE(receipt.wait_for_completion(1s));
    EXPECT_FALSE(harness.loop->has_pending_work());
    EXPECT_FALSE(harness.loop->enqueue_control([] { return true; }).accepted);
}

// 场景：多个宿主同时请求退出。期望只执行一轮 join/清队列且全部调用可返回，
// 防止并发 join 同一线程句柄；关停后 start 必须继续被拒绝。
TEST(AgentLoopShutdown, ConcurrentShutdownCallersShareOneTerminalOperation) {
    Isolation isolation;
    Harness harness(isolation);
    auto first = std::async(std::launch::async, [&harness] { harness.loop->shutdown(); });
    auto second = std::async(std::launch::async, [&harness] { harness.loop->shutdown(); });
    ASSERT_EQ(first.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(second.wait_for(2s), std::future_status::ready);
    first.get(); second.get();
    EXPECT_THROW(harness.loop->start(), std::logic_error);
}
