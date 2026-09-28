#include <gtest/gtest.h>

#include "agent/turn/busy_cycle.hpp"

#include <stdexcept>

TEST(BusyCycleScope, NormalReturnAndExplicitFinishEmitOnlyOnce) {
    int finishes = 0;
    { acecode::agent::BusyCycleScope scope([&] { ++finishes; }); }
    EXPECT_EQ(finishes, 1);
    {
        acecode::agent::BusyCycleScope scope([&] { ++finishes; });
        scope.finish();
        scope.finish();
    }
    EXPECT_EQ(finishes, 2);
}

TEST(BusyCycleScope, UnwindingLeavesTerminalReportingToWorkerRecovery) {
    int finishes = 0;
    EXPECT_THROW({
        acecode::agent::BusyCycleScope scope([&] { ++finishes; });
        throw std::runtime_error("task failed");
    }, std::runtime_error);
    EXPECT_EQ(finishes, 0);
}

TEST(BusyCycleScope, ThrowingFinishCannotBeRetriedDuringUnwinding) {
    int attempts = 0;
    EXPECT_THROW({
        acecode::agent::BusyCycleScope scope([&] {
            ++attempts;
            throw std::runtime_error("observer failed");
        });
        scope.finish();
    }, std::runtime_error);
    EXPECT_EQ(attempts, 1);
}
