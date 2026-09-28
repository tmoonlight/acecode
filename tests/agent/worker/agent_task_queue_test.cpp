#include <gtest/gtest.h>

#include "agent/control/task_handoff.hpp"
#include "agent/worker/agent_task_queue.hpp"
#include "utils/abort_signal.hpp"

#include <atomic>
#include <string>
#include <utility>
#include <vector>

namespace {
using acecode::agent::AgentTaskQueue;
using acecode::agent::WorkerTask;

WorkerTask chat(std::string text) {
    WorkerTask task;
    task.input.text = std::move(text);
    return task;
}
} // namespace

TEST(AgentTaskQueue, PreservesBothFifosAndConsumesPriorityFirst) {
    std::atomic<bool> busy{false};
    AgentTaskQueue queue(busy);
    queue.enqueue(chat("ordinary-1"));
    queue.enqueue(chat("ordinary-2"));
    queue.with_locked([](AgentTaskQueue::Locked& locked) {
        locked.push(chat("priority-1"), true);
        locked.push(chat("priority-2"), true);
    });
    std::vector<std::string> consumed;
    for (int i = 0; i < 4; ++i) {
        ASSERT_FALSE(queue.with_locked([](AgentTaskQueue::Locked& state) { return state.empty(); }));
        WorkerTask task;
        ASSERT_TRUE(queue.wait_pop(task));
        consumed.push_back(task.input.text);
        EXPECT_TRUE(queue.on_worker_thread());
        EXPECT_FALSE(queue.held_by_current_thread());
        queue.finish_task();
    }
    EXPECT_EQ(consumed, (std::vector<std::string>{
        "priority-1", "priority-2", "ordinary-1", "ordinary-2"}));
    queue.request_shutdown();
    WorkerTask unused;
    EXPECT_FALSE(queue.wait_pop(unused));
}

TEST(AgentTaskQueue, SuggestionReceiptIsIdempotentAndControlObservesQueuedTurn) {
    std::atomic<bool> busy{false};
    acecode::AbortSignal abort;
    AgentTaskQueue queue(busy);
    acecode::UserInput input;
    input.text = "suggested task";
    EXPECT_TRUE(queue.enqueue_suggestion(input, "suggestion", abort));
    EXPECT_TRUE(queue.enqueue_suggestion(input, "suggestion", abort));
    EXPECT_FALSE(queue.enqueue_suggestion(input, "different", abort));
    auto receipt = queue.enqueue_control([] { return true; });
    EXPECT_TRUE(receipt.accepted);
    EXPECT_TRUE(receipt.queued_behind_turn);
    EXPECT_FALSE(receipt.completed());
    WorkerTask task;
    ASSERT_TRUE(queue.wait_pop(task));
    EXPECT_EQ(task.input.text, "suggested task");
    queue.finish_task();
    ASSERT_FALSE(queue.with_locked([](AgentTaskQueue::Locked& state) { return state.empty(); }));
    ASSERT_TRUE(queue.wait_pop(task));
    ASSERT_EQ(task.kind, WorkerTask::Kind::Control);
    task.control();
    queue.finish_task();
    EXPECT_TRUE(receipt.applied());
    EXPECT_FALSE(queue.has_pending_work());
    queue.request_shutdown();
    EXPECT_FALSE(queue.wait_pop(task));
}

TEST(AgentTaskQueue, HandoffEntersSourceThenTargetAndReleasesBothBeforeReturning) {
    std::atomic<bool> busy{false};
    AgentTaskQueue source(busy);
    AgentTaskQueue target(busy);
    acecode::agent::TaskHandoff handoff(source);
    std::string error;
    EXPECT_TRUE(handoff.try_start_side_task([&] {
        EXPECT_TRUE(source.held_by_current_thread());
        EXPECT_FALSE(target.held_by_current_thread());
        return target.try_run_idle([&] {
            EXPECT_TRUE(source.held_by_current_thread());
            EXPECT_TRUE(target.held_by_current_thread());
        });
    }, &error));
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(source.held_by_current_thread());
    EXPECT_FALSE(target.held_by_current_thread());
}

TEST(AgentTaskQueue, HandoffPrunesOnlyHiddenChatContinuations) {
    std::atomic<bool> busy{false};
    AgentTaskQueue queue(busy);
    auto hidden = chat("goal continuation");
    hidden.hidden_goal_context = true;
    queue.enqueue(std::move(hidden));
    queue.enqueue(chat("user input"));
    queue.with_locked([](AgentTaskQueue::Locked& state) {
        state.remove_goal_continuations();
        EXPECT_TRUE(state.has_user_work());
    });
    WorkerTask task;
    ASSERT_TRUE(queue.wait_pop(task));
    EXPECT_EQ(task.input.text, "user input");
    queue.finish_task();
    EXPECT_FALSE(queue.has_pending_work());
    queue.request_shutdown();
    EXPECT_FALSE(queue.wait_pop(task));
}
