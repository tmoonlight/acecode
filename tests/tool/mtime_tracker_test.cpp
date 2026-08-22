#include <gtest/gtest.h>

#include "tool/mtime_tracker.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

TEST(MtimeTrackerTest, HumanWriteInvalidatesAgentBaselineAndReadObservation) {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto path = std::filesystem::path(testing::TempDir()) /
                      ("acecode_mtime_tracker_" + unique + ".txt");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "alpha\n";
    }

    auto& tracker = acecode::MtimeTracker::instance();
    tracker.record_read(path.string(), "alpha\n", false);
    tracker.record_read_observation(path.string(), 1, 1);

    EXPECT_EQ(
        tracker.validate_read_baseline_for_edit(path.string(), "alpha\n").status,
        acecode::MtimeTracker::ReadBaselineStatus::Ok);
    EXPECT_TRUE(tracker.has_unchanged_read_observation(path.string(), 1, 1));

    tracker.invalidate_agent_read_state(path.string());

    EXPECT_EQ(
        tracker.validate_read_baseline_for_edit(path.string(), "alpha\n").status,
        acecode::MtimeTracker::ReadBaselineStatus::NotRead);
    EXPECT_FALSE(tracker.has_unchanged_read_observation(path.string(), 1, 1));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace
