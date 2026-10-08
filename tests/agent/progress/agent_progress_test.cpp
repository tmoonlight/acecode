#include "agent/callbacks_slot.hpp"
#include <gtest/gtest.h>
#include "agent/progress/activity_narrator.hpp"
#include "agent/progress/agent_progress_emitter.hpp"
#include "agent/progress/retry_progress.hpp"
#include "utils/lifetime_token.hpp"
#include <atomic>
#include <memory>
#include <vector>

TEST(ActivityNarrator, ThinkingCallbackCanReenterAndDuplicateTitlesStaySuppressed) {
    // 标题回调查询、修改自身状态时不能再次等待同一把非递归锁。
    acecode::AgentCallbacks callbacks;
    acecode::CallbacksSlot callback_slot;
    acecode::agent::ActivityNarrator narrator(callback_slot);
    acecode::LifetimeToken lifetime;
    auto ref = lifetime.ref(narrator);
    auto calls = std::make_shared<int>(0);
    callbacks.on_thinking_title = [ref, calls](const std::string&) {
        ++*calls;
        ref.with([](auto& activity) {
            auto config = activity.config();
            config.enabled = true;
            activity.set_config(config);
            EXPECT_FALSE(activity.for_phase("model_waiting").title.empty());
        });
    };
    callback_slot.publish(callbacks);
    narrator.announce("title");
    narrator.announce("title");
    narrator.announce("next");
    EXPECT_EQ(*calls, 2);
}

TEST(AgentProgressEmitter, ExactThrottleBoundaryAndForcedUpdateUseInjectedClock) {
    // 749 ms 被节流,750 ms 立即放行;force 不等待,不使用实际 sleep。
    acecode::AgentCallbacks callbacks;
    acecode::CallbacksSlot callback_slot;
    acecode::agent::ActivityNarrator activity(callback_slot);
    acecode::ToolPreambleConfig config;
    config.enabled = false;
    activity.set_config(config);
    acecode::EventDispatcher events;
    auto received = std::make_shared<std::vector<acecode::SessionEvent>>();
    auto subscription = events.subscribe([received](const auto& event) { received->push_back(event); });
    auto milliseconds = std::make_shared<std::int64_t>(1000);
    acecode::agent::AgentProgressEmitter progress(activity, events, [milliseconds] {
        return std::chrono::steady_clock::time_point(std::chrono::milliseconds(*milliseconds));
    });
    progress.emit("reasoning", "label", "one");
    *milliseconds = 1749;
    progress.emit("reasoning", "label", "two");
    EXPECT_EQ(received->size(), 1U);
    *milliseconds = 1750;
    progress.emit("reasoning", "label", "three");
    progress.emit("reasoning", "label", "forced", {}, {}, -1, true);
    EXPECT_EQ(received->size(), 3U);
    events.unsubscribe(subscription);
}

// 触发场景:provider 因 429 限流进入退避等待;对照一条 503 过载。
// 期望行为:限流的等待文案写明「限流」,过载仍是「网络暂时不可用」;恢复文案不变。
// 回归背景(2026-10-08 反馈):内网限速触发后界面显示「网络暂时不可用,等待重试」,
// 用户去排查网络、又怀疑是上下文超限被压缩 —— 文案没说清楚是在等限流窗口。
TEST(RetryProgressReporter, RateLimitedRetryUsesDedicatedLabel) {
    acecode::AgentCallbacks callbacks;
    acecode::CallbacksSlot callback_slot;
    callback_slot.publish(callbacks);
    acecode::EventDispatcher events;
    auto labels = std::make_shared<std::vector<std::string>>();
    auto subscription = events.subscribe([labels](const acecode::SessionEvent& event) {
        labels->push_back(event.payload.value("label", std::string{}));
    });
    acecode::agent::RetryProgressReporter reporter(callback_slot, events);

    acecode::ProviderErrorInfo rate_limited;
    rate_limited.kind = acecode::ProviderErrorKind::Http;
    rate_limited.status_code = 429;
    rate_limited.retry_attempt = 1;
    rate_limited.retry_delay_ms = 1000;
    reporter.standard(rate_limited, true, false);
    reporter.standard(rate_limited, true, true);
    reporter.standard(rate_limited, false, false);

    acecode::ProviderErrorInfo overloaded = rate_limited;
    overloaded.status_code = 503;
    reporter.standard(overloaded, true, false);
    reporter.standard(overloaded, true, true);

    ASSERT_EQ(labels->size(), 5U);
    EXPECT_EQ((*labels)[0], "服务端限流，等待重试");
    EXPECT_EQ((*labels)[1], "压缩请求被服务端限流，等待重试");
    EXPECT_EQ((*labels)[2], "正在重新连接模型");
    EXPECT_EQ((*labels)[3], "网络暂时不可用，等待重试");
    EXPECT_EQ((*labels)[4], "压缩请求暂时不可用，等待重试");
    events.unsubscribe(subscription);
}

TEST(RetryProgressReporter, RegularAndCustomProgressKeepCallbackBeforeEvent) {
    acecode::AgentCallbacks callbacks;
    acecode::CallbacksSlot callback_slot;
    auto order = std::make_shared<std::vector<std::string>>();
    callbacks.on_model_retry = [order](const auto&) { order->push_back("wait"); };
    callback_slot.publish(callbacks);
    callbacks.on_model_retry_resume = [order] { order->push_back("resume"); };
    callback_slot.publish(callbacks);
    acecode::EventDispatcher events;
    auto subscription = events.subscribe([order](const auto&) { order->push_back("event"); });
    acecode::agent::RetryProgressReporter reporter(callback_slot, events);
    acecode::ProviderErrorInfo info;
    info.retry_attempt = 2;
    info.retry_delay_ms = 500;
    reporter.standard(info, true, true);
    reporter.emit(info, false, {"model_waiting", "custom", ""});
    EXPECT_EQ(*order, (std::vector<std::string>{"wait", "event", "resume", "event"}));
    events.unsubscribe(subscription);
}
