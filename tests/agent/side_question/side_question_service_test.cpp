#include <gtest/gtest.h>
#include "agent/side_question/side_question_service.hpp"
#include "test_support/agent/stub_provider.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <memory>

namespace {
class WaitingProvider : public acecode_test::StubLlmProvider {
public:
    std::promise<void> entered;
    std::promise<void> release;
    acecode::ChatResponse chat(const std::vector<acecode::ChatMessage>&,
                              const std::vector<acecode::ToolDef>& tools) override {
        EXPECT_TRUE(tools.empty());
        entered.set_value();
        release.get_future().wait();
        acecode::ChatResponse response;
        response.content = "answer";
        return response;
    }
};
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
