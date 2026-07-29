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

namespace {

class StateFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 在临时目录里给本测试一份独立 state.json 路径。
        auto tmp = fs::temp_directory_path() /
                   ("acecode_state_test_" + std::to_string(std::rand()));
        fs::create_directories(tmp);
        path_ = (tmp / "state.json").string();
        acecode::set_state_file_path_for_test(path_);
    }
    void TearDown() override {
        acecode::set_state_file_path_for_test("");
        std::error_code ec;
        fs::remove_all(fs::path(path_).parent_path(), ec);
    }
    std::string path_;
};

void write_raw(const std::string& path, const std::string& contents) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs << contents;
}

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

// 场景:同进程内多个状态写入并发发生时,read-modify-write 必须串行,不能丢 key。
TEST_F(StateFileTest, ConcurrentCheckedWritesPreserveEveryKey) {
    constexpr int kWriterCount = 12;
    std::vector<std::thread> writers;
    writers.reserve(kWriterCount);
    for (int i = 0; i < kWriterCount; ++i) {
        writers.emplace_back([i]() {
            EXPECT_TRUE(acecode::try_write_state_flag(
                "concurrent_flag_" + std::to_string(i), true));
        });
    }
    for (auto& writer : writers) writer.join();
    for (int i = 0; i < kWriterCount; ++i) {
        EXPECT_TRUE(acecode::read_state_flag("concurrent_flag_" + std::to_string(i)));
    }
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

// 场景:last_active_workspace_hash 序列化往返 — desktop 多 workspace 模型靠这条
// 跨启动持久化"上次活跃 workspace"。
TEST_F(StateFileTest, LastActiveWorkspaceHashRoundTrip) {
    EXPECT_EQ(acecode::read_last_active_workspace_hash(), ""); // 初始空
    acecode::write_last_active_workspace_hash("abc1234567890def");
    EXPECT_EQ(acecode::read_last_active_workspace_hash(), "abc1234567890def");
    // 覆盖写
    acecode::write_last_active_workspace_hash("ffffffffffffffff");
    EXPECT_EQ(acecode::read_last_active_workspace_hash(), "ffffffffffffffff");
    // 共存其他 key 不互相覆盖
    acecode::write_state_flag("some_flag", true);
    EXPECT_EQ(acecode::read_last_active_workspace_hash(), "ffffffffffffffff");
    EXPECT_TRUE(acecode::read_state_flag("some_flag"));
}

// 场景:last_active_workspace_hash 字段类型不对(数字)→ read 返回空字符串而不是抛
TEST_F(StateFileTest, LastActiveWrongTypeReadsEmpty) {
    write_raw(path_, R"({"last_active_workspace_hash": 12345})");
    EXPECT_EQ(acecode::read_last_active_workspace_hash(), "");
}

// 场景:首页 workspace 选择器跨 desktop 启动保存上次选择。
// 空字符串是有效选择,表示"不使用工作区"。
TEST_F(StateFileTest, LastHomeWorkspaceHashRoundTripAllowsEmpty) {
    EXPECT_EQ(acecode::read_last_home_workspace_hash(), "");
    acecode::write_last_home_workspace_hash("abc1234567890def");
    EXPECT_EQ(acecode::read_last_home_workspace_hash(), "abc1234567890def");

    acecode::write_last_home_workspace_hash("");
    EXPECT_EQ(acecode::read_last_home_workspace_hash(), "");

    acecode::write_state_flag("some_flag", true);
    EXPECT_EQ(acecode::read_last_home_workspace_hash(), "");
    EXPECT_TRUE(acecode::read_state_flag("some_flag"));
}

// 场景:last_home_workspace_hash 字段类型不对 → read 返回空字符串而不是抛。
TEST_F(StateFileTest, LastHomeWorkspaceWrongTypeReadsEmpty) {
    write_raw(path_, R"({"last_home_workspace_hash": 12345})");
    EXPECT_EQ(acecode::read_last_home_workspace_hash(), "");
}

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
    EXPECT_EQ(acecode::read_last_active_workspace_hash(), "abc");
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
