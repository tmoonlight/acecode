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

// 触发场景:会话 A 读过文件并记下「已读观测」;同一进程里的会话 B(daemon 里
// 另一个会话或子代理)以同样的范围读同一个没变过的文件。
// 期望行为:只有会话 A 命中「未变化」;会话 B 与不带会话的调用都查不到观测,
// 必须真的读一次。按路径失效(人工改文件)对所有会话一起生效。
// 回归背景:观测表是进程级单例、键里没有会话,会话 B 第一次读就拿到「File
// unchanged since last read」占位 —— 它从没见过那份内容,模型等于什么都没读到
// (反馈 huangyuan816 排障时发现:新会话读 SDDisplayLink.m 拿到的是别的会话留下
// 的占位)。
TEST(MtimeTrackerTest, ReadObservationsAreScopedPerSession) {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto path = std::filesystem::path(testing::TempDir()) /
                      ("acecode_mtime_scope_" + unique + ".txt");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << "alpha\n";
    }

    auto& tracker = acecode::MtimeTracker::instance();
    tracker.record_read_observation(path.string(), 0, 0, false, 0, 0,
                                    "session-a");

    EXPECT_TRUE(tracker.has_unchanged_read_observation(
        path.string(), 0, 0, false, 0, 0, "session-a"));
    EXPECT_FALSE(tracker.has_unchanged_read_observation(
        path.string(), 0, 0, false, 0, 0, "session-b"))
        << "别的会话没见过内容,不能拿到未变化占位";
    EXPECT_FALSE(tracker.has_unchanged_read_observation(path.string(), 0, 0));

    tracker.record_read_observation(path.string(), 0, 0, false, 0, 0,
                                    "session-b");
    tracker.invalidate_read_observations(path.string());
    EXPECT_FALSE(tracker.has_unchanged_read_observation(
        path.string(), 0, 0, false, 0, 0, "session-a"));
    EXPECT_FALSE(tracker.has_unchanged_read_observation(
        path.string(), 0, 0, false, 0, 0, "session-b"));

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace
