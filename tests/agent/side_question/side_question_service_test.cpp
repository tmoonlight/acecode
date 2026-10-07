#include <gtest/gtest.h>
#include "agent/side_question/side_question_service.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "test_support/agent_loop/characterization_fixture.hpp"
#include "session/request_context_record.hpp"
#include "session/session_serializer.hpp"
#include <algorithm>
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

TEST(SideQuestionService, PrimedRequestReusesFrozenContextAndRetainsReadOnlyToolWhitelist) {
    acecode_test::characterization::Isolation isolation;
    acecode_test::characterization::Harness harness(
        isolation, "side-context", {}, true, [] {
            acecode::SessionPromptConfig config;
            config.custom_instructions.emplace();
            config.custom_instructions->set_text("UNSENT_LIVE_CONTEXT");
            return config;
        });
    harness.tools.register_tool(harness.probe("file_read", true));
    harness.tools.register_tool(harness.probe("bash", true));
    harness.tools.register_tool(harness.probe("file_write", false));

    acecode::ChatMessage user;
    user.role = "user";
    user.content = "original task";
    acecode::ChatMessage snapshot;
    snapshot.role = "user";
    snapshot.is_meta = true;
    snapshot.subtype = acecode::kRequestContextSnapshot;
    snapshot.content = "FROZEN_REQUEST_CONTEXT";
    snapshot.metadata = {
        {"request_context_version", 1}, {"skills", "FROZEN_SKILL_INDEX"},
        {"context_state", {{"session", "FROZEN_REQUEST_CONTEXT"},
                           {"swarm", ""}, {"plan", ""}, {"execution", ""}}},
    };
    auto update = snapshot;
    update.subtype = acecode::kRequestContextUpdate;
    update.content = "APPENDED_REQUEST_CONTEXT";
    update.metadata = {
        {"request_context_version", 1},
        {"context_state", {{"session", "APPENDED_REQUEST_CONTEXT"}}},
    };
    // The first request snapshot is physically appended after its user input.
    const std::vector<acecode::ChatMessage> stored = {user, snapshot, update};
    for (const auto& message : stored) harness.loop->push_message(message);
    harness.loop->prime_side_question_context();
    const auto context = harness.loop->side_question_context_snapshot();
    const auto count_content = [](const std::vector<acecode::ChatMessage>& messages,
                                  const std::string& needle) {
        return std::count_if(messages.begin(), messages.end(), [&](const auto& message) {
            return message.content.find(needle) != std::string::npos;
        });
    };
    EXPECT_EQ(count_content(context, "FROZEN_REQUEST_CONTEXT"), 1);
    EXPECT_EQ(count_content(context, "APPENDED_REQUEST_CONTEXT"), 1);
    EXPECT_EQ(count_content(context, "FROZEN_SKILL_INDEX"), 1);
    EXPECT_EQ(count_content(context, "UNSENT_LIVE_CONTEXT"), 0);
    ASSERT_EQ(context.size(), 5u);
    EXPECT_EQ(context[1].content, "FROZEN_SKILL_INDEX");
    EXPECT_EQ(context[2].content, snapshot.content);
    EXPECT_EQ(context[3].content, user.content);
    EXPECT_EQ(context[4].content, update.content);

    // Even a provider that asks for a tool present in the main context cannot
    // execute it outside the side-chat whitelist.
    harness.provider->push_tool_call("bash", "{}");
    harness.provider->push_text("side answer");
    const auto answer = harness.loop->ask_side_question("explain the task");
    EXPECT_EQ(answer.status, acecode::SideQuestionStatus::Ok) << answer.error;
    EXPECT_EQ(answer.answer, "side answer");
    for (int index = 0; index < 2; ++index) {
        const auto definitions = harness.provider->tools_for_turn(index);
        ASSERT_EQ(definitions.size(), 1u);
        EXPECT_EQ(definitions.front().name, "file_read");
    }
    EXPECT_EQ(count_content(harness.provider->messages_for_turn(1),
                           "not available in this read-only side chat"), 1);
    {
        std::lock_guard<std::mutex> lock(harness.observed->mutex);
        EXPECT_TRUE(harness.observed->executions.empty());
    }
    const auto& after = harness.loop->messages();
    ASSERT_EQ(after.size(), stored.size());
    for (std::size_t index = 0; index < stored.size(); ++index) {
        EXPECT_EQ(acecode::serialize_message(after[index]),
                  acecode::serialize_message(stored[index]));
    }
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
