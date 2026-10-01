#include <gtest/gtest.h>
#include "agent/side_question/side_question_service.hpp"
#include "test_support/agent/stub_provider.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include "utils/scope_exit.hpp"

namespace {
// 一问一答的旁路问题现在与浮动侧边对话共用流式工具循环(run_side_chat)。
class WaitingProvider : public acecode_test::StubLlmProvider {
public:
    std::promise<void> entered;
    std::promise<void> release;
    void chat_stream(const std::vector<acecode::ChatMessage>&,
                     const std::vector<acecode::ToolDef>& tools,
                     const acecode::StreamCallback& callback,
                     std::atomic<bool>*) override {
        EXPECT_TRUE(tools.empty());
        entered.set_value();
        release.get_future().wait();
        acecode::StreamEvent delta;
        delta.type = acecode::StreamEventType::Delta;
        delta.content = "answer";
        callback(delta);
        acecode::StreamEvent done;
        done.type = acecode::StreamEventType::Done;
        done.finish_reason = "stop";
        callback(done);
    }
};

// 模拟一次耗时很长的模型请求(比如 provider 正在重试退避),只有中止标志能唤醒它。
class RetryWaitingProvider : public acecode_test::StubLlmProvider {
public:
    std::promise<void> entered;
    void chat_stream(const std::vector<acecode::ChatMessage>&,
                     const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback&,
                     std::atomic<bool>* abort) override {
        entered.set_value();
        EXPECT_TRUE(wait_for_retry(std::chrono::seconds(30), abort));
    }
};
}

// 场景:TUI /btw 的旁路问题正在等模型(工具循环可能还要再走好几步),此时会话关闭。
// 期望:stop_requests 直接中止在途请求,join 很快返回,结果回调被抑制。
// 回归:没有在途取消时,关闭会话要等完整个多步工具循环。
TEST(SideQuestionService, StopRequestsCancelsInFlightQuestion) {
    using namespace std::chrono_literals;
    auto provider = std::make_shared<RetryWaitingProvider>();
    auto entered = provider->entered.get_future();
    acecode::agent::SideQuestionService service([provider] { return provider; });
    acecode::ChatMessage context;
    context.role = "system";
    context.content = "context";
    service.publish({context});
    auto calls = std::make_shared<std::atomic<int>>(0);
    ASSERT_TRUE(service.ask_async("question", [calls](auto) { ++*calls; }));
    ASSERT_EQ(entered.wait_for(2s), std::future_status::ready);
    const auto started = std::chrono::steady_clock::now();
    service.stop_requests();
    service.join();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
    EXPECT_EQ(calls->load(), 0);
}

TEST(SideQuestionService, ShutdownSuppressesOutstandingCallbackAndRejectsNewRequests) {
    // 已进入 provider 的旁路请求在停机后完成时,不能再回调已关闭的界面。
    auto provider = std::make_shared<WaitingProvider>();
    auto entered = provider->entered.get_future();
    acecode::agent::SideQuestionService service([provider] { return provider; });
    acecode::ChatMessage context;
    context.role = "system";
    context.content = "context";
    service.publish({context});
    auto calls = std::make_shared<std::atomic<int>>(0);
    EXPECT_TRUE(service.ask_async("question", [calls](auto) { ++*calls; }));
    const bool started = entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    service.stop_requests();
    provider->release.set_value();
    service.join();
    EXPECT_TRUE(started);
    EXPECT_EQ(calls->load(), 0);
    EXPECT_FALSE(service.ask_async("later", {}));
}

TEST(SideQuestionService, PublishedContextIsAnIndependentValue) {
    // 返回值的修改不能污染随后请求使用的上下文。
    acecode::agent::SideQuestionService service({});
    acecode::ChatMessage context;
    context.role = "system";
    context.content = "original";
    service.publish({context});
    auto copy = service.snapshot();
    copy.front().content = "changed";
    ASSERT_EQ(service.snapshot().size(), 1U);
    EXPECT_EQ(service.snapshot().front().content, "original");
    EXPECT_EQ(service.ask(" ").status, acecode::SideQuestionStatus::InvalidQuestion);
}

// 场景：侧问的结果回调已经进入，此时关停。期望关停等待该回调退出，
// 并拒绝后续请求；仅检查 stopped 的旧逻辑无法表示在途回调寿命。
TEST(SideQuestionService, JoinWaitsForAnAlreadyAdmittedCallback) {
    using namespace std::chrono_literals;
    auto provider = std::make_shared<WaitingProvider>();
    auto entered_provider = provider->entered.get_future();
    acecode::agent::SideQuestionService service([provider] { return provider; });
    acecode::ChatMessage context; context.role = "system"; context.content = "context";
    service.publish({context});
    auto entered = std::make_shared<std::promise<void>>();
    auto ready = entered->get_future();
    auto release = std::make_shared<std::promise<void>>();
    auto released = release->get_future().share();
    acecode::ScopeExit release_on_failure([release] { try { release->set_value(); } catch (...) {} });
    ASSERT_TRUE(service.ask_async("question", [entered, released](auto) {
        entered->set_value();
        released.wait_for(3s);
    }));
    EXPECT_EQ(entered_provider.wait_for(2s), std::future_status::ready);
    provider->release.set_value();
    ASSERT_EQ(ready.wait_for(2s), std::future_status::ready);
    service.stop_requests();
    auto joined = std::async(std::launch::async, [&service] { service.join(); });
    EXPECT_EQ(joined.wait_for(30ms), std::future_status::timeout);
    release->set_value();
    ASSERT_EQ(joined.wait_for(2s), std::future_status::ready);
    joined.get();
    EXPECT_FALSE(service.ask_async("late", {}));
}
