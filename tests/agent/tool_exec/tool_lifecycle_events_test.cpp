#include <gtest/gtest.h>
#include "agent/tool_exec/tool_lifecycle_events.hpp"
#include "agent/callbacks_slot.hpp"
#include "session/event_dispatcher.hpp"
#include <memory>

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
