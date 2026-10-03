// 本文件覆盖升级诊断日志 acecode::upgrade::DiagnosticLog(src/adapters/upgrade/diagnostics.cpp):
// 每次 check / upgrade 操作一个实例,把 JSONL 记录追加到 upgrade-<UTC 日期>-<pid>.log,
// 并用 with_location() 给面向用户的错误文本补上日志位置或「诊断日志不可用」说明行。
//
// 场景:
//   1. 记录立即落盘(不等析构),同一进程同一天的多次操作追加到同一个文件。
//   2. 记录里嵌套的 URL 凭据 / 查询串 / 片段被脱敏,超长字符串被截断。
//   3. 多个线程各持一个实例并发写同一个文件,每一行都是完整 JSON。
//   4. 日志目录被同名普通文件占住:不抛异常、不报告不存在的日志文件,with_location 给出不可用说明。
//   5. 同一段错误文本经过两次 with_location(GUI 升级任务的真实调用链),不可用说明行只出现一次。
//   6. 去重只认整行完全相同:说明文字只是某一行的一部分时,照常追加本日志的说明行。

#include "upgrade/diagnostics.hpp"
#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
using acecode::upgrade::DiagnosticLog;

class UpgradeDiagnostics : public testing::Test {
protected:
    fs::path root = fs::temp_directory_path() /
        ("acecode-upgrade-log-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    void SetUp() override { fs::create_directories(root); }
    void TearDown() override { std::error_code ec; fs::remove_all(root, ec); }
};

std::vector<nlohmann::json> read_records(const std::string& path) {
    std::ifstream in(acecode::path_from_utf8(path));
    std::vector<nlohmann::json> records;
    for (std::string line; std::getline(in, line);) {
        records.push_back(nlohmann::json::parse(line));
    }
    return records;
}

// 统计 needle 在 text 里不重叠出现的次数。
size_t count_occurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (auto pos = text.find(needle); pos != std::string::npos;
         pos = text.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

// 场景 1:记录立即落盘,同一进程同一天的多次操作追加到同一个文件。
// 触发:在中文名目录下建 first 并调用 phase("verifying", ...),趁 first 还活着读文件;
//      再在同一目录建 second。
// 期望:first 存活期间文件里已有 2 条记录(operation_started + phase_started),
//      末条的 phase / details / elapsed_ms / UTC 时间('Z' 结尾)都正确;
//      second 的 operation_started 追加为第 3 条,operation_id 与 first 不同;
//      with_location 的结果包含日志路径。
TEST_F(UpgradeDiagnostics, RecordsAreFlushedBeforeDestructionAndAppendAcrossAttempts) {
    DiagnosticLog first("upgrade", root / fs::u8path(u8"日志"));
    first.phase("verifying", {{"expected_size", 123}, {"actual_size", 100}});
    auto records = read_records(first.path());
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records.back()["phase"], "verifying");
    EXPECT_EQ(records.back()["details"]["actual_size"], 100);
    EXPECT_TRUE(records.back()["elapsed_ms"].is_number());
    EXPECT_TRUE(records.back()["time"].get<std::string>().back() == 'Z');
    DiagnosticLog second("check", root / fs::u8path(u8"日志"));
    records = read_records(first.path());
    ASSERT_EQ(records.size(), 3U);
    EXPECT_NE(records.front()["operation_id"], records.back()["operation_id"]);
    EXPECT_NE(first.with_location("failed").find(first.path()), std::string::npos);
}

// 场景 2:记录内容脱敏与截断。
// 触发:record 的 details 里有带 user:password@、?token=、#fragment 的 URL(含大写 scheme、
//      数组里的 URL、IPv6 主机),以及一个 9000 字节的长字符串。
// 期望:落盘记录里找不到任何凭据 / 查询值 / 片段,主机与路径("host/pkg")保留;
//      长字符串截到 8192 字节再加 "...[truncated]" 标记,所以断言 < 8300。
TEST_F(UpgradeDiagnostics, RedactsNestedUrlsAndBoundsErrorStrings) {
    DiagnosticLog log("upgrade", root);
    log.record("failure", {{"error", "GET HTTPS://user:password@host/pkg?token=secret#fragment failed"},
                           {"urls", {"http://name@host/x?auth=hidden", "https://[::1]/pkg?token=ipv6secret"}},
                           {"long_error", std::string(9000, 'a')}});
    const auto record = read_records(log.path()).back();
    const auto text = record.dump();
    for (const auto* secret : {"password", "secret", "fragment", "hidden", "name@", "ipv6secret"}) {
        EXPECT_EQ(text.find(secret), std::string::npos) << secret;
    }
    EXPECT_NE(text.find("host/pkg"), std::string::npos);
    EXPECT_LT(record["details"]["long_error"].get<std::string>().size(), 8300U);
}

// 场景 3:多线程并发写同一个文件。
// 触发:4 个线程各建一个 DiagnosticLog(同一目录、同一进程,所以是同一个文件),各写 20 条。
// 期望:文件恰好 84 行(4 × (1 条 operation_started + 20 条 sample)),每行都能解析成 JSON ——
//      进程级 file_mutex 保证整行写入,不会交错出半行。
TEST_F(UpgradeDiagnostics, ConcurrentWritersLeaveCompleteJsonRecords) {
    std::vector<std::thread> writers;
    for (int i = 0; i < 4; ++i) {
        writers.emplace_back([&] {
            DiagnosticLog log("check", root);
            for (int j = 0; j < 20; ++j) log.record("sample", {{"number", j}});
        });
    }
    for (auto& writer : writers) writer.join();
    const auto file = fs::directory_iterator(root)->path();
    EXPECT_EQ(read_records(acecode::path_to_utf8(file)).size(), 84U);
}

// 场景 4:日志目录被同名普通文件占住。
// 触发:目录路径上是一个普通文件,DiagnosticLog 构造时 create_directories 失败。
// 期望:构造与 record 都不抛异常;path() 为空(不能把不存在的文件当日志位置报给用户);
//      error() 非空;with_location 保留原错误并追加「diagnostics unavailable」说明。
TEST_F(UpgradeDiagnostics, UnwritableDirectoryDoesNotThrowOrAdvertiseMissingFile) {
    const auto blocked = root / "blocked";
    std::ofstream(blocked) << "file";
    DiagnosticLog log("upgrade", blocked);
    EXPECT_NO_THROW(log.record("failure", {{"error", "original failure"}}));
    EXPECT_TRUE(log.path().empty());
    EXPECT_FALSE(log.error().empty());
    EXPECT_NE(log.with_location("original failure").find("original failure"), std::string::npos);
    EXPECT_NE(log.with_location("original failure").find("diagnostics unavailable"), std::string::npos);
}

// 场景 5:日志不可用时,同一段错误文本经过两次 with_location。
// 触发:日志目录被占住(同场景 4),还原 GUI 升级任务的调用链:run_upgrade_command 失败时把
//      with_location(错误) 写进 err;/api/update/start 的任务线程去掉末尾空白后,把这段文本
//      交给同一个 DiagnosticLog 再调一次 with_location,结果作为 job.error 显示在界面上。
// 期望:第一次调用的输出不变 —— 原文 + "\n" + 说明行;第二次调用原样返回第一次的结果,
//      "Upgrade diagnostics unavailable or incomplete: " 恰好出现一次;
//      文本末尾还带换行(err 流不经 trim 直接传入)时同样不再追加。
// 回归:修复前第二次调用无条件再追加一遍说明行,job.error 里这一行重复两次。
TEST_F(UpgradeDiagnostics, RepeatedWithLocationKeepsSingleUnavailableLine) {
    const auto blocked = root / "blocked";
    std::ofstream(blocked) << "file";
    DiagnosticLog log("gui_upgrade", blocked);
    ASSERT_FALSE(log.error().empty());
    const std::string marker = "Upgrade diagnostics unavailable or incomplete: ";

    const auto once = log.with_location("acecode upgrade: download failed");
    EXPECT_EQ(once, "acecode upgrade: download failed\n" + marker + log.error());

    const auto twice = log.with_location(once);
    EXPECT_EQ(twice, once);
    EXPECT_EQ(count_occurrences(twice, marker), 1U) << twice;
    EXPECT_EQ(log.with_location(once + "\n"), once + "\n");
}

// 场景 6:去重只认整行完全相同。
// 触发:日志目录被占住;消息里已有一行以说明行开头、后面还接着别的文字(如上一次尝试留下的
//      "... (previous attempt)"),或说明行前面还有别的文字("note: " + 说明行)。
// 期望:两种都不是同一行,with_location 照常在末尾追加本日志的说明行 ——
//      不能因为子串命中就把本次的日志失败原因吞掉。
TEST_F(UpgradeDiagnostics, WithLocationDedupesOnlyIdenticalUnavailableLine) {
    const auto blocked = root / "blocked";
    std::ofstream(blocked) << "file";
    DiagnosticLog log("gui_upgrade", blocked);
    ASSERT_FALSE(log.error().empty());
    const auto line = "Upgrade diagnostics unavailable or incomplete: " + log.error();

    const auto longer = "failed\n" + line + " (previous attempt)";
    EXPECT_EQ(log.with_location(longer), longer + "\n" + line);
    const auto embedded = "failed\nnote: " + line;
    EXPECT_EQ(log.with_location(embedded), embedded + "\n" + line);
}

} // namespace
