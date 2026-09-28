#include <gtest/gtest.h>

#include "agent/approval/safe_edit_guard.hpp"
#include "tool/tool_executor.hpp"

#include <chrono>

using namespace std::chrono_literals;

TEST(SafeEditGuard, RetainsTenMinuteBoundaryAndExplicitBypassDoesNotEraseFailure) {
    acecode::agent::SafeEditGuard guard;
    const auto start = acecode::agent::SafeEditGuard::Clock::time_point{} + 1s;
    guard.record_result("file_edit", "victim.txt", {"old_string did not match", false}, start);
    EXPECT_EQ(guard.blocked_path("echo x > victim.txt", false, start), "victim.txt");
    EXPECT_TRUE(guard.blocked_path("echo x > victim.txt", true, start + 1s).empty());
    EXPECT_EQ(guard.blocked_path("echo x > victim.txt", false, start + 10min), "victim.txt");
    EXPECT_TRUE(guard.blocked_path("echo x > victim.txt", false, start + 10min + 1ms).empty());
}

TEST(SafeEditGuard, IgnoresUnrelatedFailuresAndSuccessfulWrites) {
    acecode::agent::SafeEditGuard guard;
    guard.record_result("file_read", "victim.txt", {"encoding failed", false});
    guard.record_result("file_write", "victim.txt", {"disk full", false});
    guard.record_result("file_edit", "victim.txt", {"old_string replaced", true});
    EXPECT_TRUE(guard.blocked_path("echo x > victim.txt", false).empty());
}
