#include "utils/abandonable_call.hpp"
#include "utils/joining_thread.hpp"
#include "utils/scope_exit.hpp"
#include "test_support/utils/concurrency_gate.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

namespace acecode {
namespace {
using namespace std::chrono_literals;

struct DestructionProbe {
    std::shared_ptr<std::atomic<int>> destroyed;
    explicit DestructionProbe(std::shared_ptr<std::atomic<int>> count) : destroyed(std::move(count)) {}
    ~DestructionProbe() { ++*destroyed; }
};

TEST(AbandonableCallTest, ReturnsMoveOnlyResultAndPropagatesExceptions) {
    // 场景:完成的任务交付只移动结果或异常;期望原样返回,后台异常不导致 terminate。
    AbortSignal abort;
    auto result = run_abandonable<std::unique_ptr<int>>(
        [input = std::make_unique<int>(19)]() mutable { return std::move(input); }, abort);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(*result);
    EXPECT_EQ(**result, 19);
    EXPECT_THROW(run_abandonable<int>([]() -> int { throw std::runtime_error("failed"); }, abort),
                 std::runtime_error);
    EXPECT_TRUE(run_abandonable<void>([] {}, abort));
    const auto deduced = run_abandonable([] { return 31; }, abort.raw());
    ASSERT_TRUE(deduced);
    EXPECT_EQ(*deduced, 31);
    EXPECT_TRUE(wait_for_abandoned_work(2s));
}

TEST(AbandonableCallTest, NoAbortRunsInlineAndPreexistingAbortSkipsWork) {
    // 场景:无取消通道或调用前已取消;期望同步路径不派线程,预先取消不执行闭包。
    const auto caller = std::this_thread::get_id();
    const auto result = run_abandonable<std::thread::id>([] { return std::this_thread::get_id(); }, nullptr);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, caller);
    std::atomic<bool> abort{true};
    int calls = 0;
    EXPECT_FALSE(run_abandonable<int>([&calls] { return ++calls; }, abort));
    EXPECT_EQ(calls, 0);
    EXPECT_THROW(run_abandonable<int>([] { return 1; }, abort, 0ms), std::invalid_argument);
    EXPECT_TRUE(wait_for_abandoned_work(std::chrono::duration<double>(0)));
}

TEST(AbandonableCallTest, AbortReturnsWithin100msAndDiscardsLateResult) {
    // 场景:底层请求不响应取消;期望调用方在 100ms 内返回,迟到结果被释放而非交给已退出调用方。
    struct State {
        test::ConcurrencyGate entered;
        test::ConcurrencyGate release;
        test::ConcurrencyGate returned;
        std::atomic<bool> got_result{true};
        AbortSignal abort;
    };
    auto state = std::make_shared<State>();
    auto destroyed = std::make_shared<std::atomic<int>>(0);
    ScopeExit unblock([state] { state->release.open(); });
    JoiningThread caller([state, destroyed] {
        auto result = run_abandonable<std::unique_ptr<DestructionProbe>>([state, destroyed] {
            state->entered.open();
            state->release.wait();
            return std::make_unique<DestructionProbe>(destroyed);
        }, state->abort);
        state->got_result.store(result.has_value());
        state->returned.open();
    });
    EXPECT_TRUE(state->entered.wait());
    const auto start = std::chrono::steady_clock::now();
    state->abort.request();
    EXPECT_TRUE(state->returned.wait(100ms));
    EXPECT_LT(std::chrono::steady_clock::now() - start, 100ms);
    EXPECT_FALSE(state->got_result.load());
    EXPECT_EQ(destroyed->load(), 0);
    state->release.open();
    caller.join();
    EXPECT_TRUE(wait_for_abandoned_work(2s));
    EXPECT_EQ(destroyed->load(), 1);
}

TEST(AbandonableCallTest, DetachedWorkerDoesNotRetainBorrowedAbortFlag) {
    // 场景:取消后调用栈及其 atomic 已销毁,底层工作才结束;期望不再读取已释放的取消标记。
    auto release = std::make_shared<test::ConcurrencyGate>();
    auto entered = std::make_shared<test::ConcurrencyGate>();
    ScopeExit unblock([release] { release->open(); });
    {
        auto abort = std::make_shared<std::atomic<bool>>(false);
        JoiningThread cancel([entered, abort] {
            if (entered->wait()) abort->store(true);
        });
        EXPECT_FALSE(run_abandonable<int>([entered, release] {
            entered->open();
            release->wait();
            return 23;
        }, abort.get()));
    }
    release->open();
    EXPECT_TRUE(wait_for_abandoned_work(2s));
}

TEST(AbandonableCallTest, WaitIsBoundedAndEmptyRegistryReturnsImmediately) {
    // 场景:后台线程尚未完成;期望截止时间到达即返回,释放后正确归零,空集合不额外等待。
    auto release = std::make_shared<test::ConcurrencyGate>();
    auto entered = std::make_shared<test::ConcurrencyGate>();
    ScopeExit unblock([release] { release->open(); });
    spawn_owned_detached("bounded-wait-test", [entered, release] {
        entered->open();
        release->wait();
    });
    EXPECT_TRUE(entered->wait());
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(wait_for_abandoned_work(start + 20ms));
    EXPECT_LT(std::chrono::steady_clock::now() - start, 100ms);
    release->open();
    EXPECT_TRUE(wait_for_abandoned_work(2s));
    const auto empty_start = std::chrono::steady_clock::now();
    EXPECT_TRUE(wait_for_abandoned_work(2s));
    EXPECT_LT(std::chrono::steady_clock::now() - empty_start, 100ms);
}

TEST(AbandonableCallTest, DrainsCapturesEvenWhenOwnedWorkThrows) {
    // 场景:只移动闭包带有自有资源并抛异常;期望异常隔离,计数归零前已析构全部捕获。
    auto destroyed = std::make_shared<std::atomic<int>>(0);
    spawn_owned_detached("throwing-owned-work-test", [probe = std::make_unique<DestructionProbe>(destroyed)] {
        throw std::runtime_error("expected test exception");
    });
    EXPECT_TRUE(wait_for_abandoned_work(2s));
    EXPECT_EQ(destroyed->load(), 1);
}

}  // namespace
}  // namespace acecode
