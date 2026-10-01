#include "session/session_history_page.hpp"
#include "session/session_load_metrics.hpp"
#include "session/session_serializer.hpp"
#include "utils/uuid.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

namespace {
struct HistoryFixture {
    std::filesystem::path directory = std::filesystem::temp_directory_path() / ("ace-pages-" + acecode::generate_uuid());
    std::filesystem::path path = directory / "history.jsonl";
    HistoryFixture() { std::filesystem::create_directories(directory); }
    ~HistoryFixture() { std::error_code ec; std::filesystem::remove_all(directory, ec); }
    void write(const std::vector<acecode::ChatMessage>& messages) { acecode::SessionStorage::write_messages(path.string(), messages); }
};
acecode::ChatMessage message(std::string role, std::string content) {
    acecode::ChatMessage result;
    result.role = std::move(role);
    result.content = std::move(content);
    return result;
}
}

TEST(JsonlScanner, HandlesCrossBlockRecordsBlankLinesAndUnterminatedTailBothDirections) {
    HistoryFixture fixture;
    const std::string long_line(200000, 'x');
    std::ofstream(fixture.path, std::ios::binary) << "first\n\n" << long_line << "\r\ntail";
    acecode::SessionFileReader reader(fixture.path.string());
    acecode::JsonlScanner forward(reader, false), reverse(reader, true);
    std::vector<acecode::JsonlRecord> rows;
    while (auto row = forward.next()) rows.push_back(std::move(*row));
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[2].text, long_line + "\r");
    EXPECT_FALSE(rows[3].terminated);
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        auto row = reverse.next();
        ASSERT_TRUE(row);
        EXPECT_EQ(row->text, it->text);
        EXPECT_EQ(row->offset, it->offset);
        EXPECT_EQ(row->end, it->end);
        EXPECT_EQ(row->terminated, it->terminated);
    }
    EXPECT_FALSE(reverse.next());
}

TEST(SessionHistoryPage, TailAndBeforePagesExactlyReconstructVisibleHistory) {
    HistoryFixture fixture;
    std::vector<acecode::ChatMessage> all, expected;
    for (int turn = 0; turn < 47; ++turn) {
        auto user = message("user", "request-" + std::to_string(turn));
        all.push_back(user); expected.push_back(user);
        auto file = message("system", "[File checkpoint]");
        file.is_meta = true; file.subtype = "file_checkpoint"; all.push_back(file);
        auto response = message("assistant", "body mentions compact_checkpoint and file_checkpoint");
        all.push_back(response); expected.push_back(response);
        all.push_back(acecode::encode_compact_checkpoint({}));
        auto hidden = message("user", "hidden"); hidden.metadata = {{"hidden_goal_context", true}}; all.push_back(hidden);
    }
    fixture.write(all);
    acecode::SessionHistoryRequest request; request.limit = 7;
    std::vector<acecode::ChatMessage> reconstructed;
    for (int count = 0; count < 100; ++count) {
        const auto page = acecode::load_session_history_page(fixture.path.string(), request);
        ASSERT_FALSE(page.messages.empty());
        EXPECT_EQ(page.messages.front().message.role, "user");
        std::vector<acecode::ChatMessage> older;
        for (const auto& entry : page.messages) older.push_back(entry.message);
        reconstructed.insert(reconstructed.begin(), older.begin(), older.end());
        ASSERT_TRUE(acecode::decode_history_cursor(page.after));
        if (!page.has_more) { EXPECT_TRUE(page.before.empty()); break; }
        request.before = acecode::decode_history_cursor(page.before);
        ASSERT_TRUE(request.before);
    }
    ASSERT_EQ(reconstructed.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(acecode::serialize_message(reconstructed[i]), acecode::serialize_message(expected[i]));
    }
}

TEST(SessionHistoryPage, CursorSurvivesAppendButNotRewriteEvenWhenAnchorIsUnchanged) {
    HistoryFixture fixture;
    const std::vector<acecode::ChatMessage> original{message("user", "one"), message("user", "two"), message("user", "three")};
    fixture.write(original);
    acecode::SessionHistoryRequest request; request.limit = 1;
    const auto tail = acecode::load_session_history_page(fixture.path.string(), request);
    ASSERT_TRUE(tail.has_more);
    request.before = acecode::decode_history_cursor(tail.before);
    ASSERT_TRUE(acecode::SessionStorage::append_message(fixture.path.string(), message("assistant", "appended")));
    auto older = acecode::load_session_history_page(fixture.path.string(), request);
    EXPECT_EQ(older.messages.back().message.content, "two");
    fixture.write(original);
    EXPECT_THROW(acecode::load_session_history_page(fixture.path.string(), request), acecode::HistorySnapshotChanged);
    EXPECT_FALSE(acecode::decode_history_cursor("broken"));
    EXPECT_FALSE(acecode::decode_history_cursor(tail.before + ".extra"));
}

TEST(SessionHistoryPage, AfterReturnsOnlyNewRecordsAndRecoversMalformedTail) {
    HistoryFixture fixture;
    fixture.write({message("user", "first")});
    std::ofstream(fixture.path, std::ios::binary | std::ios::app) << "{broken tail";
    auto page = acecode::load_session_history_page(fixture.path.string(), {});
    ASSERT_EQ(page.messages.size(), 1u);
    acecode::SessionHistoryRequest request; request.after = acecode::decode_history_cursor(page.after);
    EXPECT_TRUE(acecode::load_session_history_page(fixture.path.string(), request).messages.empty());
    ASSERT_TRUE(acecode::SessionStorage::append_message(fixture.path.string(), message("assistant", "new")));
    page = acecode::load_session_history_page(fixture.path.string(), request);
    ASSERT_EQ(page.messages.size(), 1u);
    EXPECT_EQ(page.messages[0].message.content, "new");
    request.after = acecode::decode_history_cursor(page.after);
    EXPECT_TRUE(acecode::load_session_history_page(fixture.path.string(), request).messages.empty());
}

TEST(SessionHistoryPage, EmptyFileAfterCursorRemainsValidUntilAppend) {
    HistoryFixture fixture; fixture.write({});
    const auto page = acecode::load_session_history_page(fixture.path.string(), {});
    acecode::SessionHistoryRequest request; request.after = acecode::decode_history_cursor(page.after);
    const auto empty = acecode::load_session_history_page(fixture.path.string(), request);
    EXPECT_EQ(empty.after, page.after);
    EXPECT_TRUE(acecode::SessionStorage::append_message(fixture.path.string(), message("user", "first")));
    EXPECT_EQ(acecode::load_session_history_page(fixture.path.string(), request).messages.size(), 1u);
}

TEST(SessionHistoryPage, TailReadsOnlyItsBlocksAndBoundsAnOversizedTurn) {
    HistoryFixture fixture;
    {
        std::ofstream output(fixture.path, std::ios::binary);
        const auto line = acecode::serialize_message(message("assistant", std::string(64 * 1024, 'x'))) + "\n";
        for (int i = 0; i < 900; ++i) output << line;
    }
    const auto before = acecode::session_read_metrics();
    acecode::SessionHistoryRequest request; request.limit = 3;
    const auto page = acecode::load_session_history_page(fixture.path.string(), request);
    const auto read = acecode::session_read_metrics() - before;
    EXPECT_EQ(page.messages.size(), 3u);
    EXPECT_TRUE(page.turn_truncated);
    EXPECT_TRUE(page.has_more);
    EXPECT_LT(read.bytes, 2 * 1024 * 1024u);
    EXPECT_LE(read.records, 13u);
}

TEST(SessionResumeSuffix, UsesLatestValidCheckpointAndFallsBackWithoutCheckpoint) {
    HistoryFixture fixture;
    auto compact = acecode::CompactCheckpoint{};
    compact.replacement_history = {message("user", "retained summary")};
    auto valid = acecode::encode_compact_checkpoint(compact);
    auto invalid = valid; invalid.metadata["replacement_history"] = "damaged";
    const std::vector<acecode::ChatMessage> history{
        message("user", "old"), message("assistant", "old answer"),
        valid, message("user", "new"), invalid, message("assistant", "new answer"),
    };
    fixture.write(history);
    const auto suffix = acecode::load_session_resume_suffix(fixture.path.string());
    EXPECT_GT(suffix.start_offset, 0u);
    const auto expected = acecode::reconstruct_effective_model_history(history);
    const auto actual = acecode::reconstruct_effective_model_history(suffix.messages);
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        EXPECT_EQ(acecode::serialize_message(actual[i]), acecode::serialize_message(expected[i]));
    }
    fixture.write({message("user", "no checkpoint"), message("assistant", "answer")});
    const auto full = acecode::load_session_resume_suffix(fixture.path.string());
    EXPECT_EQ(full.start_offset, 0u);
    EXPECT_EQ(full.messages.size(), 2u);
}

TEST(SessionResumeSuffix, ReadsOnlyCheckpointSuffixOfLargeHistory) {
    HistoryFixture fixture;
    {
        std::ofstream output(fixture.path, std::ios::binary);
        const auto line = acecode::serialize_message(message("assistant", std::string(65536, 'x'))) + "\n";
        for (int i = 0; i < 900; ++i) output << line;
        acecode::CompactCheckpoint checkpoint;
        checkpoint.replacement_history = {message("user", "summary")};
        output << acecode::serialize_message(acecode::encode_compact_checkpoint(checkpoint)) << "\n";
        output << acecode::serialize_message(message("user", "latest")) << "\n";
    }
    const auto before = acecode::session_read_metrics();
    const auto result = acecode::load_session_resume_suffix(fixture.path.string());
    const auto reads = acecode::session_read_metrics() - before;
    ASSERT_EQ(result.messages.size(), 2u);
    EXPECT_GT(result.start_offset, 50 * 1024 * 1024u);
    EXPECT_LT(reads.bytes, 256 * 1024u);
    EXPECT_LE(reads.records, 3u);
}
