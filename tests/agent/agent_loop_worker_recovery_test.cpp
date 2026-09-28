#include <gtest/gtest.h>

#include "agent/agent_loop.hpp"
#include "permissions/permissions.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "tool/tool_executor.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;

TEST(AgentLoopWorkerRecovery, FailingTaskAndErrorCallbacksDoNotPreventNextTurn) {
    for (bool unknown_exception : {false, true}) {
        std::mutex mu;
        std::condition_variable cv;
        std::vector<std::string> outcomes;
        auto provider = std::make_shared<acecode_test::StubLlmProvider>();
        provider->push_text("recovered");
        acecode::ToolExecutor tools;
        acecode::PermissionManager permissions;
        acecode::AgentCallbacks callbacks;
        bool fail_next_start = true;
        callbacks.on_busy_changed = [&](bool busy) {
            if (busy && fail_next_start) {
                fail_next_start = false;
                if (unknown_exception) throw 42;
                throw std::runtime_error("startup callback failed");
            }
        };
        callbacks.on_message = [](const std::string& role, const std::string&, bool) {
            if (role == "error") throw std::runtime_error("broken error display");
        };
        callbacks.on_turn_finished = [](const std::string& status) {
            if (status == "error") throw 43;
        };
        acecode::AgentLoop loop([provider]() { return provider; }, tools,
                               callbacks, ".", permissions);
        loop.events().subscribe([&](const acecode::SessionEvent& event) {
            if (event.kind != acecode::SessionEventKind::Done) return;
            std::lock_guard<std::mutex> lock(mu);
            outcomes.push_back(event.payload.value("outcome", ""));
            cv.notify_all();
        });
        loop.submit("fail this turn");
        // Queue the next turn before the first fails: recovery must not discard it.
        loop.submit("next turn");
        {
            std::unique_lock<std::mutex> lock(mu);
            EXPECT_TRUE(cv.wait_for(lock, 10s, [&] { return outcomes.size() >= 2; }));
        }
        loop.shutdown();
        ASSERT_EQ(outcomes.size(), 2u);
        EXPECT_EQ(outcomes[0], "error");
        EXPECT_EQ(outcomes[1], "completed");
        EXPECT_EQ(provider->turn_count(), 1);
        EXPECT_FALSE(loop.has_pending_work());
        EXPECT_TRUE(loop.active_turn_id().empty());
    }
}

TEST(AgentLoopWorkerRecovery, CompactExceptionEmitsTerminalEventsOnlyOnce) {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::string> outcomes;
    int idle_events = 0;
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_text("next turn");
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    bool fail_compact = true;
    acecode::AgentLoop loop([&]() -> std::shared_ptr<acecode::LlmProvider> {
        if (fail_compact) {
            fail_compact = false;
            throw std::runtime_error("compact provider lookup failed");
        }
        return provider;
    }, tools, {}, ".", permissions);
    loop.events().subscribe([&](const acecode::SessionEvent& event) {
        std::lock_guard<std::mutex> lock(mu);
        if (event.kind == acecode::SessionEventKind::BusyChanged &&
            !event.payload.value("busy", true)) ++idle_events;
        if (event.kind == acecode::SessionEventKind::Done) {
            outcomes.push_back(event.payload.value("outcome", ""));
            cv.notify_all();
        }
    });
    loop.submit_compact();
    loop.submit("continue after compact error");
    {
        std::unique_lock<std::mutex> lock(mu);
        EXPECT_TRUE(cv.wait_for(lock, 10s, [&] { return outcomes.size() >= 2; }));
    }
    loop.shutdown();
    EXPECT_EQ(outcomes, (std::vector<std::string>{"error", "completed"}));
    EXPECT_EQ(idle_events, 2);
    EXPECT_EQ(provider->turn_count(), 1);
}
