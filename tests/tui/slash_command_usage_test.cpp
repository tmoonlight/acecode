// 场景:迁移原状态持久化用例,保持文件格式、容错与失败返回语义。
#include <gtest/gtest.h>
#include "tui/slash_command_usage.hpp"
#include "test_support/utils/state_file_fixture.hpp"

#include <cstdint>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using acecode::test_support::StateFileTest;
using acecode::test_support::write_raw;

TEST_F(StateFileTest, SlashCommandUsageIncrementsAndPreservesOtherState) {
    write_raw(path_, R"({"some_flag":true,"last_active_workspace_hash":"abc"})");

    const auto first = acecode::record_tui_slash_command_use("model");
    const auto second = acecode::record_tui_slash_command_use("model");

    EXPECT_TRUE(first.persisted);
    EXPECT_EQ(first.count, 1u);
    EXPECT_TRUE(second.persisted);
    EXPECT_EQ(second.count, 2u);
    const auto counts = acecode::read_tui_slash_command_usage();
    ASSERT_EQ(counts.size(), 1u);
    EXPECT_EQ(counts.at("model"), 2u);
    EXPECT_TRUE(acecode::read_state_flag("some_flag"));
    EXPECT_EQ(acecode::read_state_json().at("last_active_workspace_hash").get<std::string>(), "abc");
}

TEST_F(StateFileTest, SlashCommandUsageIgnoresInvalidEntries) {
    write_raw(path_,
              R"({"tui_slash_command_usage":{"good":4,"zero":0,"negative":-2,"fraction":1.5,"text":"7","bad name":9,"opsx/apply":3}})");

    const auto counts = acecode::read_tui_slash_command_usage();

    ASSERT_EQ(counts.size(), 2u);
    EXPECT_EQ(counts.at("good"), 4u);
    EXPECT_EQ(counts.at("opsx/apply"), 3u);
    EXPECT_EQ(counts.count("zero"), 0u);
    EXPECT_EQ(counts.count("negative"), 0u);
    EXPECT_EQ(counts.count("fraction"), 0u);
    EXPECT_EQ(counts.count("text"), 0u);
    EXPECT_EQ(counts.count("bad name"), 0u);

    write_raw(path_, R"({"tui_slash_command_usage":[1,2,3]})");
    EXPECT_TRUE(acecode::read_tui_slash_command_usage().empty());
}

TEST_F(StateFileTest, SlashCommandUsageSaturatesAtUint64Max) {
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    nlohmann::json state = {
        {"tui_slash_command_usage", {{"help", maximum}}},
    };
    write_raw(path_, state.dump());

    const auto result = acecode::record_tui_slash_command_use("help");

    EXPECT_TRUE(result.persisted);
    EXPECT_EQ(result.count, maximum);
    EXPECT_EQ(acecode::read_tui_slash_command_usage().at("help"), maximum);
}

TEST_F(StateFileTest, SlashCommandUsageWriteFailureReturnsInMemoryCount) {
    const fs::path directory_target =
        fs::path(path_).parent_path() / "usage-state-directory";
    fs::create_directories(directory_target);
    acecode::set_state_file_path_for_test(directory_target.string());

    const auto result = acecode::record_tui_slash_command_use("help");

    EXPECT_FALSE(result.persisted);
    EXPECT_EQ(result.count, 1u);
}
