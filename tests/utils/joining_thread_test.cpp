#include "utils/joining_thread.hpp"
#include "test_support/utils/concurrency_gate.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <type_traits>

namespace acecode {
namespace {
using namespace std::chrono_literals;

TEST(JoiningThreadTest, DestructorRequestsStopAndJoinsBeforeMembersDie) {
    // 场景:宿主退出时 worker 正在等待;期望先请求停止并 join,随后才析构依赖状态。
    struct Owner {
        std::shared_ptr<std::atomic<bool>> stopped = std::make_shared<std::atomic<bool>>(false);
        JoiningThread worker{[state = stopped](StopToken stop) {
            state->store(stop.wait_for(2s));
        }};
    };
    auto owner = std::make_unique<Owner>();
    auto stopped = owner->stopped;
    owner.reset();
    EXPECT_TRUE(stopped->load());
}

TEST(JoiningThreadTest, SelfDestructionDetachesInsteadOfThrowing) {
    // 场景:最后一个宿主引用在线程自身释放;旧裸 thread 会自 join 抛错或 terminate。
    struct State {
        test::ConcurrencyGate assigned;
        std::promise<bool> finished;
        std::unique_ptr<JoiningThread> worker;
    };
    auto state = std::make_shared<State>();
    auto finished = state->finished.get_future();
    state->worker = std::make_unique<JoiningThread>([state](StopToken stop) {
        if (!state->assigned.wait()) return;
        state->worker.reset();
        state->finished.set_value(stop.stop_requested());
    });
    state->assigned.open();
    state.reset();
    EXPECT_EQ(finished.wait_for(2s), std::future_status::ready);
    if (finished.wait_for(0ms) == std::future_status::ready) EXPECT_TRUE(finished.get());
}

TEST(JoiningThreadTest, ExplicitSelfJoinLeavesThreadNonJoinable) {
    // 场景:工作线程主动 join 自身;期望同样走有日志的 detach,外部析构安全。
    struct State {
        test::ConcurrencyGate assigned;
        std::promise<bool> finished;
        JoiningThread worker;
    };
    auto state = std::make_shared<State>();
    auto finished = state->finished.get_future();
    state->worker = JoiningThread([state] {
        if (!state->assigned.wait()) return;
        state->worker.join();
        state->finished.set_value(!state->worker.joinable());
    });
    state->assigned.open();
    EXPECT_EQ(finished.wait_for(2s), std::future_status::ready);
    if (finished.wait_for(0ms) == std::future_status::ready) EXPECT_TRUE(finished.get());
}

TEST(JoiningThreadTest, MoveAssignmentStopsOldThreadAndTransfersNewToken) {
    // 场景:覆盖仍在运行的线程句柄;期望旧线程先收尾,新 stop state 跟随新宿主。
    auto ended = std::make_shared<std::atomic<int>>(0);
    StopToken new_token;
    {
        JoiningThread first([ended](StopToken stop) { if (stop.wait_for(2s)) ++*ended; });
        auto old_token = first.get_stop_token();
        JoiningThread second([ended](StopToken stop) { if (stop.wait_for(2s)) ++*ended; });
        new_token = second.get_stop_token();
        first = std::move(second);
        EXPECT_TRUE(old_token.stop_requested());
        EXPECT_EQ(ended->load(), 1);
        EXPECT_FALSE(second.joinable());
        EXPECT_FALSE(new_token.stop_requested());
    }
    EXPECT_EQ(ended->load(), 2);
    EXPECT_TRUE(new_token.stop_requested());
}

TEST(JoiningThreadTest, SupportsMoveOnlyArgumentsAndReferenceWrappers) {
    // 场景:与 std::thread 一样传递值与显式引用;期望不复制 unique_ptr、不改变 std::ref 语义。
    int value = 0;
    JoiningThread worker([](std::unique_ptr<int> input, int& output) { output = *input; },
                         std::make_unique<int>(17), std::ref(value));
    worker.join();
    EXPECT_EQ(value, 17);
    EXPECT_FALSE(StopToken{}.stop_requested());
    EXPECT_FALSE(StopToken{}.wait_for(0ms));
}

TEST(JoiningThreadGroupTest, JoinsEveryThreadAndCanBeReused) {
    // 场景:daemon 既有线程组移入公共头;期望保持 threads/emplace_back 与 join_all 的契约。
    auto calls = std::make_shared<std::atomic<int>>(0);
    {
        JoiningThreadGroup group;
        group.threads.emplace_back([calls] { ++*calls; });
        group.threads.emplace_back([calls] { ++*calls; });
        group.join_all();
        EXPECT_TRUE(group.threads.empty());
        group.threads.emplace_back([calls] { ++*calls; });
    }
    EXPECT_EQ(calls->load(), 3);
}

TEST(ReapingThreadSetTest, ReclaimsCompletedWorkersBeforeNextSpawn) {
    // 场景:反复启动短任务;旧 vector<thread> 只增不减,新集合应回收结束的线程句柄。
    struct Completion {
        std::shared_ptr<test::ConcurrencyGate> gate;
        ~Completion() { gate->open(); }
    };
    ReapingThreadSet workers;
    for (int i = 0; i < 64; ++i) {
        auto finished = std::make_shared<test::ConcurrencyGate>();
        auto completion = std::make_unique<Completion>();
        completion->gate = finished;
        EXPECT_TRUE(workers.spawn([completion = std::move(completion)] {}));
        EXPECT_TRUE(finished->wait());
        EXPECT_EQ(workers.size(), 1u);
    }
    EXPECT_EQ(workers.reap(), 1u);
    workers.join_all();
    EXPECT_EQ(workers.size(), 0u);
}

TEST(ReapingThreadSetTest, KeepsActiveWorkersParallelAndClosesAdmissionAtShutdown) {
    // 场景:多个任务同时阻塞在 stop token;期望不串行化,shutdown 唤醒全部并拒绝新任务。
    auto entered = std::make_shared<std::atomic<int>>(0);
    auto stopped = std::make_shared<std::atomic<int>>(0);
    ReapingThreadSet workers;
    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(workers.spawn([entered, stopped](StopToken stop) {
            ++*entered;
            if (stop.wait_for(2s)) ++*stopped;
        }));
    }
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (entered->load() < 8 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    EXPECT_EQ(entered->load(), 8);
    EXPECT_EQ(workers.size(), 8u);
    workers.shutdown();
    EXPECT_EQ(stopped->load(), 8);
    EXPECT_EQ(workers.size(), 0u);
    EXPECT_FALSE(workers.spawn([] {}));
    workers.shutdown();
}

TEST(ReapingThreadSetTest, DestructionFromOwnTaskDoesNotTouchDestroyedSet) {
    // 场景:任务持有最后一份集合所有权;期望自析构保护生效,完成标记不再访问宿主。
    struct Owner {
        test::ConcurrencyGate release;
        std::promise<void> finished;
        std::unique_ptr<ReapingThreadSet> workers = std::make_unique<ReapingThreadSet>();
    };
    auto owner = std::make_shared<Owner>();
    auto finished = owner->finished.get_future();
    EXPECT_TRUE(owner->workers->spawn([owner] {
        if (!owner->release.wait()) return;
        owner->workers.reset();
        owner->finished.set_value();
    }));
    owner->release.open();
    owner.reset();
    EXPECT_EQ(finished.wait_for(2s), std::future_status::ready);
}

}  // namespace
}  // namespace acecode
