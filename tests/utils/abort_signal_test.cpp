#include "utils/abort_signal.hpp"
#include "utils/joining_thread.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <type_traits>
#include <vector>

namespace acecode {
namespace {
using namespace std::chrono_literals;

TEST(AbortSignalTest, RequestWakesEveryWaiterAndClearResetsState) {
    // 场景:多个等待者与 request 竞争;期望全部被唤醒,清除后重新等待。
    auto signal = std::make_shared<AbortSignal>();
    auto woke = std::make_shared<std::atomic<int>>(0);
    std::vector<JoiningThread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back([signal, woke] {
            if (signal->wait_for(2s)) ++*woke;
        });
    }
    const auto start = std::chrono::steady_clock::now();
    signal->request();
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(woke->load(), 8);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
    EXPECT_TRUE(signal->raw().load());
    signal->clear();
    EXPECT_FALSE(signal->wait_for(1ms));
    EXPECT_FALSE(signal->raw().load());
}

TEST(AbortSignalTest, RequestBeforeWaitIsNotLostAndLegacyFlagIsBorrowed) {
    // 场景:取消先于等待,兼容 API 读写原始 atomic;期望不丢通知且只暴露非 const 借用。
    AbortSignal signal;
    static_assert(std::is_same_v<decltype(std::as_const(signal).raw()), const std::atomic<bool>&>);
    signal.request();
    EXPECT_TRUE(signal.wait_for(0ms));
    signal.clear();
    signal.flag_for_legacy_api().store(true);
    EXPECT_TRUE(signal.wait_for(0ms));
}

}  // namespace
}  // namespace acecode
