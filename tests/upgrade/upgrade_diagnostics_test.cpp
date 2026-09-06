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

} // namespace
