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

// 场景:侧边对话在旁路读取作用域里读文件,且主代理此前已读过同一范围。
// 期望:作用域内既不登记基线 / 观察,也看不到主代理留下的「未变化」观察(旁路读
// 必须拿到真内容);离开作用域后主代理的状态原样可用,且旁路读取没有留下任何
// 记录。回归:作用域缺失时,主代理随后读同一文件只拿到它没见过内容的短桩。
TEST(MtimeTrackerTest, DetachedReadScopeNeitherRecordsNorReusesAgentReadState) {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto agent_path = std::filesystem::path(testing::TempDir()) /
                            ("acecode_mtime_detached_agent_" + unique + ".txt");
    const auto side_path = std::filesystem::path(testing::TempDir()) /
                           ("acecode_mtime_detached_side_" + unique + ".txt");
    for (const auto& path : {agent_path, side_path}) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "alpha\n";
    }

    auto& tracker = acecode::MtimeTracker::instance();
    tracker.record_read_observation(agent_path.string(), 1, 1);
    {
        acecode::MtimeTracker::DetachedReadScope detached;
        EXPECT_TRUE(acecode::MtimeTracker::DetachedReadScope::active());
        EXPECT_FALSE(tracker.has_unchanged_read_observation(agent_path.string(), 1, 1));
        tracker.record_read(side_path.string(), "alpha\n", false);
        tracker.record_read_observation(side_path.string(), 1, 1);
    }
    EXPECT_FALSE(acecode::MtimeTracker::DetachedReadScope::active());
    EXPECT_TRUE(tracker.has_unchanged_read_observation(agent_path.string(), 1, 1));
    EXPECT_FALSE(tracker.has_unchanged_read_observation(side_path.string(), 1, 1));
    EXPECT_EQ(
        tracker.validate_read_baseline_for_edit(side_path.string(), "alpha\n").status,
        acecode::MtimeTracker::ReadBaselineStatus::NotRead);

    tracker.invalidate_agent_read_state(agent_path.string());
    std::error_code ec;
    std::filesystem::remove(agent_path, ec);
    std::filesystem::remove(side_path, ec);
}

} // namespace
