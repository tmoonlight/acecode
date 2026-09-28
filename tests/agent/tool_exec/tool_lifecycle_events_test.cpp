#include <gtest/gtest.h>
#include "agent/tool_exec/tool_lifecycle_events.hpp"
#include "agent/callbacks_slot.hpp"
#include "session/event_dispatcher.hpp"
#include <memory>
#include <future>
#include "utils/joining_thread.hpp"
#include "utils/scope_exit.hpp"

TEST(ToolLifecycleEvents, CompletedStreamCannotDeliverRetainedChunks) {
    acecode::EventDispatcher events;
    acecode::AgentCallbacks callbacks;
    acecode::CallbacksSlot callback_slot;
    auto updates = std::make_shared<int>(0);
    callbacks.on_tool_progress_update = [updates](const auto&...) { ++*updates; };
    callback_slot.publish(callbacks);
    acecode::ToolContext context;
    {
        acecode::agent::ToolLifecycleEvents::Stream stream(events, callback_slot,
            acecode::ToolCall{"id", "bash", "{}"}, 0, true, {},
            std::chrono::steady_clock::now());
        stream.bind(context);
        context.stream("first");
    }
    const auto last_seq = events.current_seq();
    context.stream("retained");
    EXPECT_EQ(events.current_seq(), last_seq);
    EXPECT_EQ(*updates, 1);
}

// 场景：流式工具回调已进入时销毁调用作用域。期望等待在途回调，
// 返回后保留的弱回调不能再访问事件分发器或界面。
TEST(ToolLifecycleEvents, StreamDestructionDrainsInFlightDelivery) {
    using namespace std::chrono_literals;
    acecode::EventDispatcher events;
    acecode::CallbacksSlot callbacks;
    auto entered = std::make_shared<std::promise<void>>();
    auto ready = entered->get_future();
    auto release = std::make_shared<std::promise<void>>();
    auto released = release->get_future().share();
    acecode::AgentCallbacks values;
    values.on_tool_progress_update = [entered, released](const auto&...) {
        entered->set_value(); released.wait_for(3s);
    };
    callbacks.publish(std::move(values));
    acecode::ToolContext context;
    auto stream = std::make_unique<acecode::agent::ToolLifecycleEvents::Stream>(
        events, callbacks, acecode::ToolCall{"id", "bash", "{}"}, 0, true,
        acecode::agent::ToolLifecycleEvents::Clock{}, std::chrono::steady_clock::now());
    stream->bind(context);
    acecode::JoiningThread emitter([send = context.stream] { send("in flight"); });
    acecode::ScopeExit cleanup([release] { try { release->set_value(); } catch (...) {} });
    ASSERT_EQ(ready.wait_for(2s), std::future_status::ready);
    auto destroyed = std::async(std::launch::async, [owned = std::move(stream)]() mutable { owned.reset(); });
    EXPECT_EQ(destroyed.wait_for(30ms), std::future_status::timeout);
    release->set_value();
    ASSERT_EQ(destroyed.wait_for(2s), std::future_status::ready);
    destroyed.get();
    emitter.join();
    const auto sequence = events.current_seq();
    context.stream("late");
    EXPECT_EQ(events.current_seq(), sequence);
}
