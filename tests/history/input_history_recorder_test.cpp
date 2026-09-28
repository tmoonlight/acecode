#include <gtest/gtest.h>
#include "history/input_history_recorder.hpp"
#include "history/input_history_store.hpp"
#include "config/config.hpp"
#include "session/session_storage.hpp"
#include "utils/utf8_path.hpp"
#include "test_support/agent_loop/characterization_fixture.hpp"

TEST(InputHistoryRecorder, BlankAndAdjacentDuplicatesAreSuppressedOnly) {
    // 只抑制空白与相邻重复;隔了一条之后再次输入必须保留。
    acecode::InputHistoryConfig config;
    config.enabled = false;
    std::vector<std::string> history;
    for (const auto* entry : {"", " \t\n", "first", "first", "!echo hi", "first"}) {
        acecode::record_input_history(history, config, "", entry);
    }
    EXPECT_EQ(history, (std::vector<std::string>{"first", "!echo hi", "first"}));
}
TEST(InputHistoryRecorder, DiskLimitDoesNotRetroactivelyTrimInMemoryHistory) {
    // 当前内存列表沿用旧逻辑,只有磁盘 append 负责上限截断。
    acecode_test::characterization::Isolation isolation;
    acecode_test::characterization::TemporaryDirectory workspace;
    acecode::InputHistoryConfig config;
    config.enabled = true;
    config.max_entries = 2;
    std::vector<std::string> history;
    const auto cwd = acecode::path_to_utf8(workspace.path);
    for (const auto* entry : {"one", "two", "three"}) {
        acecode::record_input_history(history, config, cwd, entry);
    }
    EXPECT_EQ(history, (std::vector<std::string>{"one", "two", "three"}));
    const auto path = acecode::InputHistoryStore::file_path(
        acecode::SessionStorage::get_project_dir(cwd));
    EXPECT_EQ(acecode::InputHistoryStore::load(path), (std::vector<std::string>{"two", "three"}));
}
