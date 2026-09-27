#include "utils/lifetime_token.hpp"
#include "utils/joining_thread.hpp"
#include "utils/scope_exit.hpp"
#include "test_support/utils/concurrency_gate.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace acecode {
namespace {
using namespace std::chrono_literals;

TEST(LifetimeTokenTest, RevokeClosesAdmissionThenWaitsForInFlightCallback) {
    // 场景:回调已准入但尚未返回时撤销;期望新回调被拒,撤销必须等原回调结束。
    struct State {
        test::ConcurrencyGate entered;
        test::ConcurrencyGate release;
        test::ConcurrencyGate revoked;
        std::atomic<int> calls{0};
        LifetimeToken token;
    };
    auto state = std::make_shared<State>();
    auto ref = state->token.ref(*state);
    ScopeExit unblock([state] { state->release.open(); });
    JoiningThread callback([ref] {
        ref.with([](State& value) {
            ++value.calls;
            value.entered.open();
            value.release.wait();
        });
    });
    EXPECT_TRUE(state->entered.wait());
    JoiningThread revoker([state] {
        state->token.revoke();
        state->revoked.open();
    });
    bool admitted = true;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (admitted && std::chrono::steady_clock::now() < deadline) {
        admitted = ref.with([](State&) {});
        std::this_thread::yield();
    }
    EXPECT_FALSE(admitted);
    EXPECT_FALSE(state->revoked.wait(1ms));
    state->release.open();
    EXPECT_TRUE(state->revoked.wait());
    callback.join();
    revoker.join();
    EXPECT_FALSE(ref.with([](State& value) { ++value.calls; }));
    EXPECT_EQ(state->calls.load(), 1);
}

TEST(LifetimeTokenTest, CallbackExceptionAndNestedCallsReleaseAdmission) {
    // 场景:嵌套准入后抛异常;期望计数由 RAII 全部归还,撤销不死锁且业务代码无内部锁。
    LifetimeToken token;
    int value = 0;
    auto ref = token.ref(value);
    EXPECT_THROW(ref.with([ref](int& outer) {
        ++outer;
        EXPECT_TRUE(ref.with([](int& inner) { ++inner; }));
        throw std::runtime_error("callback failed");
    }), std::runtime_error);
    token.revoke();
    EXPECT_EQ(value, 2);
    EXPECT_FALSE(ref.with([](int&) {}));
}

TEST(LifetimeTokenTest, DestructorRevokesCopiedRefsAndMoveTransfersDuty) {
    // 场景:令牌转移与宿主退出后仍保留 ref;期望不重复撤销旧令牌,销毁新令牌后拒绝访问。
    int value = 0;
    LifetimeRef<int> ref;
    {
        LifetimeToken original;
        ref = original.ref(value);
        LifetimeToken moved(std::move(original));
        original.revoke();
        EXPECT_TRUE(ref.with([](int& owner) { ++owner; }));
        LifetimeToken assigned;
        int previous = 0;
        auto previous_ref = assigned.ref(previous);
        assigned = std::move(moved);
        EXPECT_FALSE(previous_ref.with([](int&) {}));
        EXPECT_TRUE(ref.with([](int& owner) { ++owner; }));
    }
    EXPECT_EQ(value, 2);
    EXPECT_FALSE(ref.with([](int&) {}));
    EXPECT_FALSE(LifetimeRef<int>{}.with([](int&) {}));
    static_assert(!std::is_copy_constructible_v<LifetimeToken>);
    static_assert(std::is_copy_constructible_v<LifetimeRef<int>>);
}

TEST(LifetimeTokenTest, AdmissionRaceNeverCallsOwnerAfterRevokeReturns) {
    // 场景:多个线程同时反复准入并与 revoke 竞争;期望返回后不再有在途或新增业务回调。
    struct State {
        test::ConcurrencyGate start;
        std::atomic<bool> revoked{false};
        std::atomic<int> invalid_calls{0};
        LifetimeToken token;
    };
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto state = std::make_shared<State>();
        auto ref = state->token.ref(*state);
        std::vector<JoiningThread> workers;
        for (int i = 0; i < 4; ++i) {
            workers.emplace_back([state, ref] {
                if (!state->start.wait()) return;
                for (int call = 0; call < 64; ++call) {
                    ref.with([](State& owner) {
                        if (owner.revoked.load()) ++owner.invalid_calls;
                        std::this_thread::yield();
                        if (owner.revoked.load()) ++owner.invalid_calls;
                    });
                }
            });
        }
        state->start.open();
        state->token.revoke();
        state->revoked.store(true);
        for (auto& worker : workers) worker.join();
        EXPECT_EQ(state->invalid_calls.load(), 0);
        EXPECT_FALSE(ref.with([](State&) {}));
    }
}

#if GTEST_HAS_DEATH_TEST
TEST(LifetimeTokenDeathTest, RejectsRevocationInsideOwnNestedCallback) {
    // 场景:回调撤销自己的令牌;这是约定禁止的用法,debug 断言/release fail-fast 代替永久等待。
    EXPECT_DEATH({
        LifetimeToken token;
        int value = 0;
        auto ref = token.ref(value);
        ref.with([ref, &token](int&) {
            ref.with([&token](int&) { token.revoke(); });
        });
    }, ".*");
}
#endif

}  // namespace
}  // namespace acecode
