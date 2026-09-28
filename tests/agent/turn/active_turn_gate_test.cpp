#include <gtest/gtest.h>

#include "agent/turn/active_turn_gate.hpp"
#include "agent/worker/agent_task_queue.hpp"
#include "utils/abort_signal.hpp"
#include "utils/joining_thread.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {
using acecode::agent::ActiveTurnGate;
using acecode::agent::AgentTaskQueue;
using acecode::agent::WorkerTask;

acecode::UserInput input(std::string text) {
    acecode::UserInput value;
    value.text = std::move(text);
    return value;
}
} // namespace

TEST(ActiveTurnGate, FailedInterruptCapacityCheckLeavesAcceptedSteersIntact) {
    std::atomic<bool> busy{true}, interrupted{false};
    acecode::AbortSignal abort;
    AgentTaskQueue queue(busy);
    ActiveTurnGate gate(busy, abort, interrupted);
    gate.begin("turn");
    for (std::size_t i = 0; i < ActiveTurnGate::kMaxPendingInputs; ++i) {
        ASSERT_TRUE(gate.steer("turn", input(std::to_string(i))).accepted());
    }
    EXPECT_EQ(gate.steer("turn", input("overflow")).status, acecode::TurnSteerStatus::QueueFull);
    std::size_t promised = 0;
    EXPECT_EQ(gate.interrupt("turn", input("interrupt"), queue, promised).status,
              acecode::TurnSteerStatus::QueueFull);
    EXPECT_EQ(promised, 0u);
    EXPECT_FALSE(abort.raw().load());
    EXPECT_FALSE(interrupted.load());
    EXPECT_EQ(gate.id(), "turn");
    auto drained = gate.drain(true);
    ASSERT_EQ(drained.inputs.size(), ActiveTurnGate::kMaxPendingInputs);
    EXPECT_EQ(drained.inputs.front().text, "0");
    EXPECT_EQ(drained.inputs.back().text, "127");
    EXPECT_TRUE(gate.drain(true).inputs.empty());
    EXPECT_TRUE(gate.id().empty());
}

TEST(ActiveTurnGate, InterruptMovesEveryAcceptedInputExactlyOnceAheadOfOrdinaryWork) {
    std::atomic<bool> busy{true}, interrupted{false};
    acecode::AbortSignal abort;
    AgentTaskQueue queue(busy);
    ActiveTurnGate gate(busy, abort, interrupted);
    WorkerTask ordinary;
    ordinary.input = input("ordinary");
    queue.enqueue(std::move(ordinary));
    gate.begin("turn");
    ASSERT_TRUE(gate.steer("turn", input("steer-1")).accepted());
    ASSERT_TRUE(gate.steer("turn", input("steer-2")).accepted());
    std::size_t promised = 0;
    ASSERT_TRUE(gate.interrupt("turn", input("interrupt"), queue, promised).accepted());
    EXPECT_EQ(promised, 3u);
    EXPECT_TRUE(abort.raw().load());
    EXPECT_TRUE(interrupted.load());
    EXPECT_FALSE(gate.interrupt("turn", input("duplicate"), queue, promised).accepted());
    EXPECT_TRUE(gate.drain(false).inputs.empty());
    std::vector<std::string> consumed;
    for (int i = 0; i < 4; ++i) {
        ASSERT_FALSE(queue.with_locked([](AgentTaskQueue::Locked& state) { return state.empty(); }));
        WorkerTask task;
        ASSERT_TRUE(queue.wait_pop(task));
        consumed.push_back(task.input.text);
        if (i < 3) EXPECT_EQ(task.input.metadata.value("interrupted_turn_id", ""), "turn");
        queue.finish_task();
    }
    EXPECT_EQ(consumed, (std::vector<std::string>{"steer-1", "steer-2", "interrupt", "ordinary"}));
    queue.request_shutdown();
    WorkerTask unused;
    EXPECT_FALSE(queue.wait_pop(unused));
}

TEST(ActiveTurnGate, FinalBoundaryRaceNeverDropsAnAcknowledgedInput) {
    for (int iteration = 0; iteration < 64; ++iteration) {
        std::atomic<bool> busy{true}, interrupted{false};
        acecode::AbortSignal abort;
        ActiveTurnGate gate(busy, abort, interrupted);
        gate.begin("turn");
        std::mutex mu;
        std::condition_variable cv;
        bool start = false;
        acecode::TurnSteerResult result;
        ActiveTurnGate::DrainedInputs first;
        auto await_start = [&] {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait(lock, [&] { return start; });
        };
        // Both JoiningThreads are joined before any borrowed local is destroyed.
        acecode::JoiningThread accept([&] {
            await_start();
            result = gate.steer("turn", input("accepted input"));
        });
        acecode::JoiningThread close([&] {
            await_start();
            first = gate.drain(true);
        });
        {
            std::lock_guard<std::mutex> lock(mu);
            start = true;
        }
        cv.notify_all();
        accept.join();
        close.join();
        const auto last = gate.drain(true);
        EXPECT_EQ(first.inputs.size() + last.inputs.size(), result.accepted() ? 1u : 0u);
    }
}
