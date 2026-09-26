#include "utils/scope_exit.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <type_traits>

namespace acecode {
namespace {

TEST(ScopeExitTest, CleansUpOnReturnAndException) {
    // 场景:正常退出或异常展开;期望均执行一次收尾,避免早退漏清理。
    int calls = 0;
    {
        ScopeExit cleanup([&calls] { ++calls; });
    }
    EXPECT_EQ(calls, 1);
    EXPECT_THROW({
        ScopeExit cleanup([&calls] { ++calls; });
        throw std::runtime_error("unwind");
    }, std::runtime_error);
    EXPECT_EQ(calls, 2);
}

TEST(ScopeExitTest, MoveTransfersCleanupAndReleaseCancelsIt) {
    // 场景:转移只移动闭包的守卫;期望原守卫不重复执行,release 后不再执行。
    int total = 0;
    {
        ScopeExit original([value = std::make_unique<int>(7), &total] { total += *value; });
        static_assert(!std::is_copy_constructible_v<decltype(original)>);
        auto moved = std::move(original);
    }
    EXPECT_EQ(total, 7);
    {
        ScopeExit original([&total] { ++total; });
        auto moved = std::move(original);
        moved.release();
    }
    EXPECT_EQ(total, 7);
}

}  // namespace
}  // namespace acecode
