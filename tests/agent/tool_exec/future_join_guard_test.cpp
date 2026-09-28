#include "utils/future_join_guard.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <future>
#include <memory>
#include <stdexcept>

namespace acecode::utils {
namespace {

TEST(FutureJoinGuard, UnwindingWaitsForUnconsumedCalls) {
    std::promise<void> release;
    auto released = release.get_future();
    auto finished = std::make_shared<std::atomic<bool>>(false);
    try {
        FutureJoinGuard<int> calls;
        calls.add(std::async(std::launch::async,
            [released = std::move(released), finished]() mutable {
                released.get();
                finished->store(true);
                return 7;
            }));
        release.set_value();
        throw std::runtime_error("display callback");
    } catch (const std::runtime_error&) {
        EXPECT_TRUE(finished->load());
    }
}

TEST(FutureJoinGuard, AFailedResultDoesNotLoseAnotherResult) {
    FutureJoinGuard<int> calls;
    const auto failed = calls.add(std::async(std::launch::async, []() -> int {
        throw std::runtime_error("tool failure");
    }));
    const auto completed = calls.add(std::async(std::launch::async, [] { return 42; }));
    EXPECT_THROW(calls.get(failed), std::runtime_error);
    EXPECT_EQ(calls.get(completed), 42);
    calls.join();
}

} // namespace
} // namespace acecode::utils
