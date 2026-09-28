#include <gtest/gtest.h>

#include "agent/agent_callbacks.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/turn/turn_outcome.hpp"
#include "agent/worker/agent_task_queue.hpp"
#include "permissions/permissions.hpp"
#include "session/event_dispatcher.hpp"
#include "utils/abort_signal.hpp"
#include "utils/joining_thread.hpp"
#include "utils/lifetime_token.hpp"

#include <atomic>
#include <memory>
#include <vector>

TEST(GoalRuntime, ConcurrentStatusObserversCanReenterWithoutHoldingCursorLock) {
    std::atomic<bool> busy{true};
    acecode::AbortSignal abort;
    acecode::AgentCallbacks callbacks;
    acecode::PermissionManager permissions;
    acecode::EventDispatcher events;
    acecode::agent::ConversationHistory history(busy);
    acecode::agent::TurnOutcomeRecord outcome;
    acecode::agent::TranscriptWriter transcript(history, events, callbacks, outcome);
    acecode::agent::AgentTaskQueue queue(busy);
    acecode::agent::GoalRuntime runtime(queue, history, transcript, events, callbacks, permissions, busy, abort);
    acecode::LifetimeToken lifetime;
    const auto owner = lifetime.ref(runtime);
    auto calls = std::make_shared<std::atomic<int>>(0);
    callbacks.on_goal_status = [owner, calls](const std::string& status) {
        calls->fetch_add(1);
        if (!status.empty()) owner.with([](auto& goal) { goal.emit_cleared("session"); });
    };
    acecode::ThreadGoal active;
    active.thread_id = "session";
    active.goal_id = "goal";
    active.objective = "exercise reentrant observers";
    active.status = acecode::ThreadGoalStatus::Active;
    std::vector<acecode::JoiningThread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([owner, active] {
            for (int n = 0; n < 20; ++n) {
                owner.with([&active](auto& goal) { goal.emit_updated(active); });
            }
        });
    }
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(calls->load(), 160);
}
