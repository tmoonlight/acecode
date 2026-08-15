#include "session/session_trajectory.hpp"
#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

class SessionTrajectoryStorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
            fs::path("acecode-trajectory-" +
                     std::to_string(std::chrono::steady_clock::now()
                         .time_since_epoch().count()));
        fs::create_directories(root_);
        path_ = acecode::SessionTrajectoryStorage::file_path(
            acecode::path_to_utf8(root_), "session-a");
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    acecode::SessionTrajectoryRecord record(std::uint64_t sequence,
                                             std::string type = "message") {
        acecode::SessionTrajectoryRecord value;
        value.sequence = sequence;
        value.timestamp_ms = 1000 + static_cast<std::int64_t>(sequence);
        value.type = std::move(type);
        value.payload = {{"value", sequence}};
        return value;
    }

    fs::path root_;
    std::string path_;
};

TEST_F(SessionTrajectoryStorageTest, AppendsAndPagesInSequenceOrder) {
    for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
        ASSERT_TRUE(acecode::SessionTrajectoryStorage::append(
            path_, record(sequence)));
    }

    auto first = acecode::SessionTrajectoryStorage::load_page(path_, 0, 2);
    ASSERT_EQ(first.records.size(), 2u);
    EXPECT_EQ(first.records[0].sequence, 1u);
    EXPECT_EQ(first.records[1].sequence, 2u);
    EXPECT_EQ(first.next_after, 2u);
    EXPECT_TRUE(first.has_more);

    auto second = acecode::SessionTrajectoryStorage::load_page(
        path_, first.next_after, 2);
    ASSERT_EQ(second.records.size(), 2u);
    EXPECT_EQ(second.records[0].sequence, 3u);
    EXPECT_EQ(second.records[1].sequence, 4u);
    EXPECT_EQ(second.next_after, 4u);
    EXPECT_FALSE(second.has_more);
    EXPECT_EQ(acecode::SessionTrajectoryStorage::last_sequence(path_), 4u);
}

TEST_F(SessionTrajectoryStorageTest, SkipsMalformedCompleteRecord) {
    ASSERT_TRUE(acecode::SessionTrajectoryStorage::append(path_, record(1)));
    {
        std::ofstream output(acecode::path_from_utf8(path_),
                             std::ios::binary | std::ios::app);
        output << "{broken}\n";
    }
    ASSERT_TRUE(acecode::SessionTrajectoryStorage::append(path_, record(2)));

    auto page = acecode::SessionTrajectoryStorage::load_page(path_);
    ASSERT_EQ(page.records.size(), 2u);
    EXPECT_EQ(page.diagnostics.malformed_complete_records, 1u);
    EXPECT_FALSE(page.diagnostics.ignored_partial_tail);
}

TEST_F(SessionTrajectoryStorageTest, IgnoresPartialTailAndIsolatesNextAppend) {
    ASSERT_TRUE(acecode::SessionTrajectoryStorage::append(path_, record(1)));
    {
        std::ofstream output(acecode::path_from_utf8(path_),
                             std::ios::binary | std::ios::app);
        output << "{\"schema_version\":1";
    }

    auto damaged = acecode::SessionTrajectoryStorage::load_page(path_);
    ASSERT_EQ(damaged.records.size(), 1u);
    EXPECT_TRUE(damaged.diagnostics.ignored_partial_tail);

    ASSERT_TRUE(acecode::SessionTrajectoryStorage::append(path_, record(2)));
    auto recovered = acecode::SessionTrajectoryStorage::load_page(path_);
    ASSERT_EQ(recovered.records.size(), 2u);
    EXPECT_EQ(recovered.records.back().sequence, 2u);
    EXPECT_EQ(recovered.diagnostics.malformed_complete_records, 1u);
}

TEST_F(SessionTrajectoryStorageTest, RecoversValidUnterminatedRecord) {
    const auto encoded = acecode::session_trajectory_record_to_json(record(7)).dump();
    fs::create_directories(acecode::path_from_utf8(path_).parent_path());
    {
        std::ofstream output(acecode::path_from_utf8(path_), std::ios::binary);
        output << encoded;
    }

    auto page = acecode::SessionTrajectoryStorage::load_page(path_);
    ASSERT_EQ(page.records.size(), 1u);
    EXPECT_EQ(page.records.front().sequence, 7u);
    EXPECT_TRUE(page.diagnostics.recovered_unterminated_record);
    EXPECT_EQ(acecode::SessionTrajectoryStorage::last_sequence(path_), 7u);
}

TEST(SessionTrajectoryRecordTest, RejectsInvalidRequiredFields) {
    acecode::SessionTrajectoryRecord record;
    EXPECT_FALSE(acecode::session_trajectory_record_from_json(
        nlohmann::json{{"schema_version", 1},
                       {"sequence", 0},
                       {"timestamp_ms", 10},
                       {"type", "message"}},
        &record));
    EXPECT_FALSE(acecode::session_trajectory_record_from_json(
        nlohmann::json{{"schema_version", 1},
                       {"sequence", 1},
                       {"timestamp_ms", -1},
                       {"type", "message"}},
        &record));
}

} // namespace
