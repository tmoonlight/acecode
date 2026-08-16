#include "desktop/startup_progress.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace acecode::desktop {
namespace {

class FakeClock {
public:
    explicit FakeClock(std::vector<std::uint64_t> values)
        : values_(std::move(values)) {}

    std::uint64_t next() {
        if (offset_ >= values_.size()) return values_.back();
        return values_[offset_++];
    }

private:
    std::vector<std::uint64_t> values_;
    std::size_t offset_ = 0;
};

} // namespace

TEST(DesktopStartupProgress, RecordsMonotonicSequenceAndElapsedTime) {
    FakeClock clock({1000, 1017, 1005, 1042});
    DesktopStartupTimeline timeline([&] { return clock.next(); });

    auto first = timeline.record("config_load_begin", "loading");
    auto second = timeline.record("workspace_scan_begin", "scanning");
    auto third = timeline.record("ui_ready", "ready", "frontend", true, 39.5);

    EXPECT_EQ(first.sequence, 1u);
    EXPECT_EQ(first.elapsed_ms, 17u);
    EXPECT_EQ(second.sequence, 2u);
    EXPECT_EQ(second.elapsed_ms, 17u);
    EXPECT_EQ(third.sequence, 3u);
    EXPECT_EQ(third.elapsed_ms, 42u);
    EXPECT_TRUE(third.terminal);
    ASSERT_TRUE(third.frontend_ms.has_value());
    EXPECT_DOUBLE_EQ(*third.frontend_ms, 39.5);
}

TEST(DesktopStartupProgress, SerializesBoundedPrivacySafeSnapshot) {
    std::uint64_t now = 500;
    DesktopStartupTimeline timeline([&] { return now++; });
    for (int i = 0; i < 40; ++i) {
        timeline.record("stage_" + std::to_string(i), "message");
    }

    const auto snapshot = nlohmann::json::parse(timeline.snapshot_json());
    EXPECT_EQ(snapshot.at("version"), kDesktopStartupProgressVersion);
    ASSERT_EQ(snapshot.at("history").size(), 32u);
    EXPECT_EQ(snapshot.at("current").at("sequence"), 40u);
    EXPECT_EQ(snapshot.at("current").at("stage"), "stage_39");
    EXPECT_EQ(snapshot.dump().find("cwd"), std::string::npos);
    EXPECT_EQ(snapshot.dump().find("token"), std::string::npos);
}

TEST(DesktopStartupProgress, KeepsTerminalSnapshotCurrentAfterLatePaintMilestone) {
    std::uint64_t now = 700;
    DesktopStartupTimeline timeline([&] { return now++; });
    timeline.record("ui_ready", "ready", "frontend", true, 520.0);
    timeline.record(
        "first_contentful_paint", "painted", "frontend", false, 444.0);

    const auto snapshot = nlohmann::json::parse(timeline.snapshot_json());
    EXPECT_EQ(snapshot.at("current").at("stage"), "ui_ready");
    EXPECT_TRUE(snapshot.at("current").at("terminal"));
    ASSERT_EQ(snapshot.at("history").size(), 2u);
    EXPECT_EQ(snapshot.at("history").back().at("stage"),
              "first_contentful_paint");
}

TEST(DesktopStartupProgress, ParsesAllowedFrontendMilestone) {
    std::string error;
    auto parsed = parse_frontend_startup_milestone(
        R"([{"stage":"first_contentful_paint","performance_ms":125.75}])",
        &error);

    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->stage, "first_contentful_paint");
    ASSERT_TRUE(parsed->performance_ms.has_value());
    EXPECT_DOUBLE_EQ(*parsed->performance_ms, 125.75);
}

TEST(DesktopStartupProgress, RejectsUnknownOrInvalidFrontendMilestone) {
    std::string error;
    EXPECT_FALSE(parse_frontend_startup_milestone(
        R"([{"stage":"arbitrary_log","performance_ms":1}])", &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(parse_frontend_startup_milestone(
        R"([{"stage":"ui_ready","performance_ms":90000000}])", &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(parse_frontend_startup_milestone("[]", &error));
    EXPECT_FALSE(error.empty());
}

TEST(DesktopStartupProgress, LocalizesKnownStagesAndMarksTerminalStage) {
    EXPECT_EQ(desktop_startup_stage_message("daemon_activate_begin", "zh-CN"),
              u8"正在启动后台服务…");
    EXPECT_EQ(desktop_startup_stage_message("daemon_activate_begin", "en-US"),
              "Starting background service...");
    EXPECT_TRUE(is_frontend_startup_stage("daemon_connected"));
    EXPECT_FALSE(is_frontend_startup_stage("workspace_scan_begin"));
    EXPECT_TRUE(is_terminal_startup_stage("ui_ready"));
    EXPECT_FALSE(is_terminal_startup_stage("dom_ready"));
}

} // namespace acecode::desktop
