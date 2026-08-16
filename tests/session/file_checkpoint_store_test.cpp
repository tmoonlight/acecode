#include <gtest/gtest.h>

#include "session/file_checkpoint_store.hpp"
#include "session/turn_net_diff.hpp"

#include <filesystem>
#include <fstream>
#include <cstdint>
#include <iterator>
#include <limits>
#include <random>
#include <set>
#include <string>

namespace fs = std::filesystem;

using acecode::FileCheckpointStore;

namespace {

fs::path make_temp_dir(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
               ("acecode_checkpoint_" + hint + "_" +
                std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary);
    ofs << content;
}

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

} // namespace

TEST(FileCheckpointStore, ExistingFileBackupDiffAndRestore) {
    auto project = make_temp_dir("restore_existing");
    auto file = project / "work.txt";
    write_file(file, "a\nb\n");

    FileCheckpointStore store;
    store.set_session(project.string(), "s1");
    store.make_snapshot("u1");
    ASSERT_TRUE(store.track_before_write(file.string()).has_value());

    write_file(file, "a\nx\n");

    auto stats = store.diff_stats("u1");
    EXPECT_TRUE(stats.has_changes());
    ASSERT_EQ(stats.files_changed.size(), 1u);
    EXPECT_TRUE(stats.errors.empty());

    auto restored = store.rewind_to("u1");
    EXPECT_TRUE(restored.ok());
    ASSERT_EQ(restored.files_changed.size(), 1u);
    EXPECT_EQ(read_file(file), "a\nb\n");

    fs::remove_all(project);
}

TEST(FileCheckpointStore, AbsentMarkerDeletesCreatedFile) {
    auto project = make_temp_dir("delete_created");
    auto file = project / "new.txt";

    FileCheckpointStore store;
    store.set_session(project.string(), "s1");
    store.make_snapshot("u1");
    ASSERT_TRUE(store.track_before_write(file.string()).has_value());

    write_file(file, "created\n");
    ASSERT_TRUE(fs::exists(file));

    auto restored = store.rewind_to("u1");
    EXPECT_TRUE(restored.ok());
    EXPECT_FALSE(fs::exists(file));

    fs::remove_all(project);
}

TEST(FileCheckpointStore, TrackBeforeWriteCapturesOncePerSnapshot) {
    auto project = make_temp_dir("once");
    auto file = project / "once.txt";
    write_file(file, "original\n");

    FileCheckpointStore store;
    store.set_session(project.string(), "s1");
    store.make_snapshot("u1");

    auto first = store.track_before_write(file.string());
    ASSERT_TRUE(first.has_value());
    write_file(file, "mid\n");

    auto second = store.track_before_write(file.string());
    EXPECT_FALSE(second.has_value());
    write_file(file, "final\n");

    auto restored = store.rewind_to("u1");
    EXPECT_TRUE(restored.ok());
    EXPECT_EQ(read_file(file), "original\n");

    fs::remove_all(project);
}

TEST(FileCheckpointStore, EncodeDecodeSnapshotMetaRoundtrip) {
    acecode::FileCheckpointSnapshot snapshot;
    snapshot.uuid = "snapshot-1";
    snapshot.message_uuid = "user-1";
    snapshot.timestamp = "2026-04-26T00:00:00Z";
    snapshot.tracked_file_backups["/tmp/a.txt"] =
        acecode::FileCheckpointBackup{"hash@v1", 1, "2026-04-26T00:00:00Z", false};

    auto msg = FileCheckpointStore::encode_snapshot_message(snapshot);
    EXPECT_EQ(msg.role, "system");
    EXPECT_TRUE(msg.is_meta);
    EXPECT_EQ(msg.subtype, "file_checkpoint");

    auto decoded = FileCheckpointStore::decode_snapshot_message(msg);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->uuid, "snapshot-1");
    EXPECT_EQ(decoded->message_uuid, "user-1");
    ASSERT_EQ(decoded->tracked_file_backups.size(), 1u);
    EXPECT_EQ(decoded->tracked_file_backups.begin()->second.backup_file_name, "hash@v1");
}

TEST(FileCheckpointStore, LoadFromMessagesReconstructsCheckpointState) {
    auto project = make_temp_dir("load");
    auto file = project / "load.txt";
    write_file(file, "old\n");

    FileCheckpointStore store;
    store.set_session(project.string(), "s1");
    auto snapshot = store.make_snapshot("u1");
    snapshot = *store.track_before_write(file.string());
    auto meta = FileCheckpointStore::encode_snapshot_message(snapshot);

    FileCheckpointStore loaded;
    loaded.load_from_messages(project.string(), "s1", {meta});
    EXPECT_TRUE(loaded.can_restore("u1"));

    write_file(file, "new\n");
    auto restored = loaded.rewind_to("u1");
    EXPECT_TRUE(restored.ok());
    EXPECT_EQ(read_file(file), "old\n");

    fs::remove_all(project);
}

TEST(FileCheckpointStore, ForkCopiesRetainedBackupsAndDropsDiscardedSnapshots) {
    auto project = make_temp_dir("fork");
    auto file = project / "fork.txt";
    write_file(file, "old\n");

    FileCheckpointStore store;
    store.set_session(project.string(), "s1");
    store.make_snapshot("u1");
    ASSERT_TRUE(store.track_before_write(file.string()).has_value());
    write_file(file, "mid\n");

    store.make_snapshot("u2");
    ASSERT_TRUE(store.track_before_write(file.string()).has_value());
    write_file(file, "new\n");

    auto meta = store.fork_to_session("s2", std::set<std::string>{"u1"});

    ASSERT_EQ(meta.size(), 1u);
    EXPECT_TRUE(store.can_restore("u1"));
    EXPECT_FALSE(store.can_restore("u2"));

    auto restored = store.rewind_to("u1");
    EXPECT_TRUE(restored.ok());
    EXPECT_EQ(read_file(file), "old\n");

    fs::remove_all(project);
}

TEST(FileCheckpointStore, MissingBackupReportsError) {
    auto project = make_temp_dir("missing");
    auto file = project / "missing.txt";
    write_file(file, "old\n");

    FileCheckpointStore store;
    store.set_session(project.string(), "s1");
    store.make_snapshot("u1");
    auto snapshot = store.track_before_write(file.string());
    ASSERT_TRUE(snapshot.has_value());

    const auto backup = snapshot->tracked_file_backups.begin()->second.backup_file_name;
    fs::remove(fs::path(store.checkpoint_dir()) / backup);
    write_file(file, "new\n");

    auto stats = store.diff_stats("u1");
    EXPECT_FALSE(stats.errors.empty());

    auto restored = store.rewind_to("u1");
    EXPECT_FALSE(restored.ok());
    EXPECT_FALSE(restored.errors.empty());

    fs::remove_all(project);
}

TEST(FileCheckpointStore, SnapshotCapEvictsOldestMetadata) {
    auto project = make_temp_dir("cap");

    FileCheckpointStore store(1);
    store.set_session(project.string(), "s1");
    store.make_snapshot("u1");
    store.make_snapshot("u2");

    ASSERT_EQ(store.snapshots().size(), 1u);
    EXPECT_FALSE(store.can_restore("u1"));
    EXPECT_TRUE(store.can_restore("u2"));

    fs::remove_all(project);
}

TEST(FileCheckpointStore, ActiveTurnNetDiffUsesOriginalBaselineAfterRepeatedWrites) {
    auto project = make_temp_dir("turn_net_repeated");
    auto file = project / "repeat.txt";
    write_file(file, "original\n");

    FileCheckpointStore store;
    store.set_session(project.string(), "s1");
    store.make_snapshot("u1");
    ASSERT_TRUE(store.track_before_write(file.string()).has_value());
    write_file(file, "middle\n");
    EXPECT_FALSE(store.track_before_write(file.string()).has_value());
    write_file(file, "final\nvalue\n");

    const auto diff = store.build_active_turn_net_diff(project.string());
    EXPECT_TRUE(diff.complete);
    EXPECT_EQ(diff.user_message_uuid, "u1");
    ASSERT_EQ(diff.files.size(), 1u);
    EXPECT_EQ(diff.files[0].file, "repeat.txt");
    EXPECT_EQ(diff.files[0].additions, 2);
    EXPECT_EQ(diff.files[0].deletions, 1);
    ASSERT_EQ(diff.files[0].hunks.size(), 1u);

    fs::remove_all(project);
}

TEST(FileCheckpointStore, ActiveTurnNetDiffHandlesCreatedDeletedAndRevertedFiles) {
    auto project = make_temp_dir("turn_net_states");
    auto created = project / "created.txt";
    auto removed = project / "removed.txt";
    auto reverted = project / "reverted.txt";
    write_file(removed, "remove\nme\n");
    write_file(reverted, "same\n");

    FileCheckpointStore store;
    store.set_session(project.string(), "s1");
    store.make_snapshot("u1");
    ASSERT_TRUE(store.track_before_write(created.string()).has_value());
    write_file(created, "one\ntwo\n");
    ASSERT_TRUE(store.track_before_write(removed.string()).has_value());
    fs::remove(removed);
    ASSERT_TRUE(store.track_before_write(reverted.string()).has_value());
    write_file(reverted, "temporary\n");
    write_file(reverted, "same\n");

    const auto diff = store.build_active_turn_net_diff(project.string());
    EXPECT_TRUE(diff.complete);
    ASSERT_EQ(diff.files.size(), 2u);
    EXPECT_EQ(diff.files[0].file, "created.txt");
    EXPECT_EQ(diff.files[0].additions, 2);
    EXPECT_EQ(diff.files[0].deletions, 0);
    EXPECT_EQ(diff.files[1].file, "removed.txt");
    EXPECT_EQ(diff.files[1].additions, 0);
    EXPECT_EQ(diff.files[1].deletions, 2);

    fs::remove_all(project);
}

TEST(FileCheckpointStore, ActiveTurnNetDiffReportsMissingBackupAsIncomplete) {
    auto project = make_temp_dir("turn_net_missing");
    auto file = project / "missing.txt";
    write_file(file, "old\n");

    FileCheckpointStore store;
    store.set_session(project.string(), "s1");
    store.make_snapshot("u1");
    auto snapshot = store.track_before_write(file.string());
    ASSERT_TRUE(snapshot.has_value());
    const auto backup = snapshot->tracked_file_backups.begin()->second.backup_file_name;
    fs::remove(fs::path(store.checkpoint_dir()) / backup);
    write_file(file, "new\n");

    const auto diff = store.build_active_turn_net_diff(project.string());
    EXPECT_FALSE(diff.complete);
    EXPECT_TRUE(diff.files.empty());
    EXPECT_FALSE(diff.errors.empty());

    fs::remove_all(project);
}

TEST(TurnNetDiff, MetadataRoundtripAndInvalidHunkFallback) {
    acecode::TurnNetDiffRecord record;
    record.user_message_uuid = "user-1";
    record.files.push_back({"src/a.cpp", 1, 0,
                            acecode::generate_structured_diff("", "line\n", "src/a.cpp")});

    const auto message = acecode::make_turn_net_diff_message(
        record, "2026-08-15T00:00:00Z");
    EXPECT_TRUE(acecode::is_turn_net_diff_message(message));
    EXPECT_TRUE(message.metadata.value("transcript_only", false));
    auto decoded = acecode::decode_turn_net_diff(message.metadata["turn_net_diff"]);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->user_message_uuid, "user-1");
    ASSERT_EQ(decoded->files.size(), 1u);
    EXPECT_EQ(decoded->files[0].additions, 1);

    auto invalid = message.metadata["turn_net_diff"];
    invalid["files"][0]["hunks"][0]["lines"][0]["kind"] = "unknown";
    EXPECT_FALSE(acecode::decode_turn_net_diff(invalid).has_value());

    invalid = message.metadata["turn_net_diff"];
    invalid["files"][0]["additions"] = std::numeric_limits<std::uint64_t>::max();
    EXPECT_FALSE(acecode::decode_turn_net_diff(invalid).has_value());

    invalid = message.metadata["turn_net_diff"];
    invalid["files"][0]["hunks"][0]["old_start"] =
        std::numeric_limits<std::uint64_t>::max();
    EXPECT_FALSE(acecode::decode_turn_net_diff(invalid).has_value());
}
