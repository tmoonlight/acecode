// 覆盖 src/utils/state_file.cpp 的 read_state_flag / write_state_flag。
// 通过 set_state_file_path_for_test 把目标路径切到 GoogleTest 临时目录,避免
// 污染用户真实 ~/.acecode/state.json,也保证测试间相互隔离。
//
// 主要场景:
//   - 文件不存在 → read 返回 false
//   - write 之后 read 拿到对应值
//   - 多个 key 互不覆盖
//   - 损坏的 JSON → read 返回 false,后续 write 成功覆盖
//   - 非对象 JSON(数组 / 标量)同样视为损坏

#include <gtest/gtest.h>

#include "utils/state_file.hpp"
#include "test_support/utils/state_file_fixture.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using acecode::test_support::StateFileTest;
using acecode::test_support::write_raw;

namespace {

std::string quote_arg(const std::string& value) {
    return "\"" + value + "\"";
}

int run_claim_worker(const std::string& state_path,
                     const std::string& key,
                     const std::string& ready_path,
                     const std::string& go_path,
                     const std::string& result_path) {
    std::ostringstream command;
#ifdef _WIN32
    command << "\"";
#endif
    command
        << quote_arg(ACECODE_STATE_FILE_CLAIM_WORKER_PATH) << " "
        << quote_arg(state_path) << " "
        << quote_arg(key) << " "
        << quote_arg(ready_path) << " "
        << quote_arg(go_path) << " "
        << quote_arg(result_path);
#ifdef _WIN32
    command << "\"";
#endif
    return std::system(command.str().c_str());
}

std::pair<int, int> read_claim_result(const fs::path& path) {
    std::ifstream input(path);
    int claimed = -1;
    int persisted = -1;
    input >> claimed >> persisted;
    return {claimed, persisted};
}

} // namespace

// 场景:文件不存在 → read 返回 false,不抛异常
TEST_F(StateFileTest, MissingFileReadsFalse) {
    EXPECT_FALSE(fs::exists(path_));
    EXPECT_FALSE(acecode::read_state_flag("legacy_terminal_hint_shown"));
}

// 场景:write 之后 read 立刻看到 true
TEST_F(StateFileTest, WriteThenReadTrue) {
    acecode::write_state_flag("legacy_terminal_hint_shown", true);
    EXPECT_TRUE(fs::exists(path_));
    EXPECT_TRUE(acecode::read_state_flag("legacy_terminal_hint_shown"));
}

// 场景:需要向 API 报告落盘结果时,checked write 明确返回成功。
TEST_F(StateFileTest, CheckedWriteReportsSuccess) {
    EXPECT_TRUE(acecode::try_write_state_flag("desktop_guided_tour_v1_dismissed", true));
    EXPECT_TRUE(acecode::read_state_flag("desktop_guided_tour_v1_dismissed"));
}

// 场景:目标路径本身是目录,原子 rename 无法覆盖,checked write 返回 false。
TEST_F(StateFileTest, CheckedWriteReportsFailure) {
    fs::path directory_target = fs::path(path_).parent_path() / "state-directory";
    fs::create_directories(directory_target);
    acecode::set_state_file_path_for_test(directory_target.string());
    EXPECT_FALSE(acecode::try_write_state_flag("desktop_guided_tour_v1_dismissed", true));
}

// 场景:write false 也写入(显式 reset 用)
TEST_F(StateFileTest, WriteFalseExplicitlyStored) {
    acecode::write_state_flag("legacy_terminal_hint_shown", true);
    acecode::write_state_flag("legacy_terminal_hint_shown", false);
    EXPECT_FALSE(acecode::read_state_flag("legacy_terminal_hint_shown"));
}

// 场景:写入新 key 不应覆盖已有 key
TEST_F(StateFileTest, MultipleKeysCoexist) {
    acecode::write_state_flag("legacy_terminal_hint_shown", true);
    acecode::write_state_flag("another_flag", true);
    EXPECT_TRUE(acecode::read_state_flag("legacy_terminal_hint_shown"));
    EXPECT_TRUE(acecode::read_state_flag("another_flag"));
}

TEST_F(StateFileTest, ClaimFlagIsGrantedExactlyOnceAcrossConcurrentCallers) {
    constexpr int kClaimerCount = 16;
    std::atomic<int> claimed{0};
    std::atomic<int> persisted{0};
    std::vector<std::thread> claimers;
    claimers.reserve(kClaimerCount);
    for (int i = 0; i < kClaimerCount; ++i) {
        claimers.emplace_back([&]() {
            const auto result =
                acecode::try_claim_state_flag(
                    "connector_first_start_auth_v1");
            if (result.claimed) ++claimed;
            if (result.persisted) ++persisted;
        });
    }
    for (auto& claimer : claimers) claimer.join();

    EXPECT_EQ(claimed.load(), 1);
    EXPECT_EQ(persisted.load(), kClaimerCount);
    EXPECT_TRUE(acecode::read_state_flag(
        "connector_first_start_auth_v1"));
    const auto later = acecode::try_claim_state_flag(
        "connector_first_start_auth_v1");
    EXPECT_FALSE(later.claimed);
    EXPECT_TRUE(later.persisted);
}

TEST_F(StateFileTest, ClaimFlagIsGrantedExactlyOnceAcrossProcesses) {
    const fs::path directory = fs::path(path_).parent_path();
    const fs::path ready_a = directory / "ready-a";
    const fs::path ready_b = directory / "ready-b";
    const fs::path go = directory / "go";
    const fs::path result_a = directory / "result-a";
    const fs::path result_b = directory / "result-b";
    const std::string key = "connector_first_start_auth_v1";

    auto worker_a = std::async(std::launch::async, [&]() {
        return run_claim_worker(
            path_, key, ready_a.string(), go.string(), result_a.string());
    });
    auto worker_b = std::async(std::launch::async, [&]() {
        return run_claim_worker(
            path_, key, ready_b.string(), go.string(), result_b.string());
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((!fs::exists(ready_a) || !fs::exists(ready_b)) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    write_raw(go.string(), "go\n");

    ASSERT_TRUE(fs::exists(ready_a));
    ASSERT_TRUE(fs::exists(ready_b));
    EXPECT_EQ(worker_a.get(), 0);
    EXPECT_EQ(worker_b.get(), 0);

    const auto [claimed_a, persisted_a] = read_claim_result(result_a);
    const auto [claimed_b, persisted_b] = read_claim_result(result_b);
    EXPECT_EQ(claimed_a + claimed_b, 1);
    EXPECT_EQ(persisted_a, 1);
    EXPECT_EQ(persisted_b, 1);
    EXPECT_TRUE(acecode::read_state_flag(key));
}

TEST_F(StateFileTest, FailedClaimIsNotGranted) {
    fs::path directory_target =
        fs::path(path_).parent_path() / "claim-state-directory";
    fs::create_directories(directory_target);
    acecode::set_state_file_path_for_test(directory_target.string());

    const auto result = acecode::try_claim_state_flag(
        "connector_first_start_auth_v1");

    EXPECT_FALSE(result.claimed);
    EXPECT_FALSE(result.persisted);
}

// 场景:已存在但内容是非合法 JSON → read 视同 false,后续 write 覆盖成功
TEST_F(StateFileTest, CorruptedJsonReadsFalseAndWriteOverwrites) {
    write_raw(path_, "this is not json {{{");
    EXPECT_FALSE(acecode::read_state_flag("legacy_terminal_hint_shown"));

    acecode::write_state_flag("legacy_terminal_hint_shown", true);
    EXPECT_TRUE(acecode::read_state_flag("legacy_terminal_hint_shown"));
}

// 场景:JSON 合法但顶层是数组(不是对象)→ 视同损坏,read=false
TEST_F(StateFileTest, NonObjectJsonTreatedAsCorrupted) {
    write_raw(path_, "[1, 2, 3]");
    EXPECT_FALSE(acecode::read_state_flag("legacy_terminal_hint_shown"));
}

// 场景:key 存在但 value 不是 bool(比如字符串)→ read=false
TEST_F(StateFileTest, NonBoolValueReadsFalse) {
    write_raw(path_, R"({"legacy_terminal_hint_shown": "yes"})");
    EXPECT_FALSE(acecode::read_state_flag("legacy_terminal_hint_shown"));
}

// 场景:文件为空 → read=false,write 之后正常工作
TEST_F(StateFileTest, EmptyFileTreatedAsEmptyState) {
    write_raw(path_, "");
    EXPECT_FALSE(acecode::read_state_flag("legacy_terminal_hint_shown"));
    acecode::write_state_flag("legacy_terminal_hint_shown", true);
    EXPECT_TRUE(acecode::read_state_flag("legacy_terminal_hint_shown"));
}
