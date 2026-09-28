#include <gtest/gtest.h>
#include "agent/tool_exec/tool_lifecycle_events.hpp"
#include "agent/agent_callbacks.hpp"
#include "session/event_dispatcher.hpp"
#include <memory>

TEST(ToolLifecycleEvents, CompletedStreamCannotDeliverRetainedChunks) {
    acecode::EventDispatcher events;
    acecode::AgentCallbacks callbacks;
    auto updates = std::make_shared<int>(0);
    callbacks.on_tool_progress_update = [updates](const auto&...) { ++*updates; };
    acecode::ToolContext context;
    {
        acecode::agent::ToolLifecycleEvents::Stream stream(events, callbacks,
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
