#include <gtest/gtest.h>

#include "llm/llm_provider.hpp"
#include "session/session_manager.hpp"
#include "session/session_rewind.hpp"
#include "session/session_storage.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using acecode::ChatMessage;
using acecode::SessionManager;
using acecode::SessionStorage;

namespace {

fs::path make_temp_cwd(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
               ("acecode_session_rewind_" + hint + "_" +
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

ChatMessage user_msg(const std::string& uuid, const std::string& content) {
    ChatMessage msg;
    msg.role = "user";
    msg.uuid = uuid;
    msg.timestamp = "2026-04-26T00:00:00Z";
    msg.content = content;
    return msg;
}

bool contains_content(const std::vector<ChatMessage>& messages, const std::string& content) {
    for (const auto& msg : messages) {
        if (msg.content == content) return true;
    }
    return false;
}

} // namespace

TEST(SessionManagerRewind, ForkWritesTrimmedSessionAndKeepsOriginalListed) {
    auto cwd = make_temp_cwd("fork");
    auto project_dir = SessionStorage::get_project_dir(cwd.string());
    fs::remove_all(project_dir);

    SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model");

    ChatMessage first = user_msg("u1", "first prompt");
    sm.on_message(first);
    sm.begin_user_turn_checkpoint("u1");

    ChatMessage tail = user_msg("u2", "discarded tail");
    sm.on_message(tail);

    const std::string old_id = sm.current_session_id();
    ASSERT_FALSE(old_id.empty());

    std::string new_id = sm.fork_active_session({first});
    ASSERT_FALSE(new_id.empty());
    EXPECT_NE(new_id, old_id);

    auto sessions = sm.list_sessions();
    bool saw_old = false;
    bool saw_new = false;
    for (const auto& meta : sessions) {
        if (meta.id == old_id) saw_old = true;
        if (meta.id == new_id) saw_new = true;
    }
    EXPECT_TRUE(saw_old);
    EXPECT_TRUE(saw_new);

    auto new_messages = SessionStorage::load_messages(
        SessionStorage::session_path(project_dir, new_id));
    EXPECT_TRUE(contains_content(new_messages, "first prompt"));
    EXPECT_FALSE(contains_content(new_messages, "discarded tail"));

    auto resumed = sm.resume_session(new_id);
    EXPECT_TRUE(contains_content(resumed, "first prompt"));
    EXPECT_FALSE(contains_content(resumed, "discarded tail"));

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}

TEST(SessionManagerRewind, LoopOriginPersistsButDoesNotPropagateToFork) {
    auto cwd = make_temp_cwd("loop_origin");
    auto project_dir = SessionStorage::get_project_dir(cwd.string());
    fs::remove_all(project_dir);

    SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model");
    sm.set_loop_origin("loop-1", "run-1");

    ChatMessage first = user_msg("u1", "scheduled prompt");
    sm.on_message(first);
    const std::string original_id = sm.current_session_id();
    ASSERT_FALSE(original_id.empty());

    auto original_meta = SessionStorage::read_meta(
        SessionStorage::meta_path(project_dir, original_id));
    EXPECT_EQ(original_meta.loop_id, "loop-1");
    EXPECT_EQ(original_meta.loop_run_id, "run-1");

    const std::string fork_id = sm.fork_active_session({first});
    ASSERT_FALSE(fork_id.empty());
    auto fork_meta = SessionStorage::read_meta(
        SessionStorage::meta_path(project_dir, fork_id));
    EXPECT_TRUE(fork_meta.loop_id.empty());
    EXPECT_TRUE(fork_meta.loop_run_id.empty());

    ASSERT_FALSE(sm.resume_session(original_id).empty());
    sm.set_input_draft("keep provenance through metadata rewrites");
    original_meta = SessionStorage::read_meta(
        SessionStorage::meta_path(project_dir, original_id));
    EXPECT_EQ(original_meta.loop_id, "loop-1");
    EXPECT_EQ(original_meta.loop_run_id, "run-1");

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}

TEST(SessionManagerRewind, ResumeReconstructsCheckpointState) {
    auto cwd = make_temp_cwd("resume");
    auto project_dir = SessionStorage::get_project_dir(cwd.string());
    fs::remove_all(project_dir);
    auto file = cwd / "tracked.txt";
    write_file(file, "old\n");

    SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model");

    ChatMessage first = user_msg("u1", "edit file");
    sm.on_message(first);
    sm.begin_user_turn_checkpoint("u1");
    sm.track_file_write_before(file.string());
    write_file(file, "new\n");

    const std::string session_id = sm.current_session_id();
    ASSERT_FALSE(session_id.empty());

    auto messages = sm.resume_session(session_id);
    ASSERT_FALSE(messages.empty());

    write_file(file, "latest\n");
    auto restored = sm.rewind_files_to_checkpoint("u1");

    EXPECT_TRUE(restored.ok());
    EXPECT_EQ(read_file(file), "old\n");

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}

TEST(SessionManagerRewind, SuffixResumeLazilyRestoresFilesFromBeforeLatestCompact) {
    auto cwd = make_temp_cwd("suffix");
    const auto project = SessionStorage::get_project_dir(cwd.string());
    auto file = cwd / "tracked.txt";
    write_file(file, "original\n");
    SessionManager sm;
    sm.start_session(cwd.string(), "test", "test");
    sm.on_message(user_msg("before", "old prompt"));
    sm.begin_user_turn_checkpoint("before");
    sm.track_file_write_before(file.string());
    write_file(file, "modified\n");
    acecode::CompactCheckpoint compact;
    compact.window_number = 5;
    compact.window_id = "window-five";
    compact.first_window_id = "window-one";
    compact.replacement_history = {user_msg("summary", "summary")};
    ASSERT_TRUE(sm.append_compact_checkpoint(compact));
    sm.on_message(user_msg("after", "new prompt"));
    const auto id = sm.current_session_id();
    const auto count = sm.load_session_meta(id).message_count;
    const auto suffix = sm.resume_session(id, true);
    EXPECT_FALSE(contains_content(suffix, "old prompt"));
    EXPECT_TRUE(contains_content(suffix, "new prompt"));
    EXPECT_EQ(sm.load_session_meta(id).message_count, count);
    const auto latest = sm.load_latest_compact_checkpoint();
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->window_number, 5u);
    EXPECT_EQ(latest->first_window_id, "window-one");
    EXPECT_TRUE(sm.file_checkpoint_can_restore("before"));
    EXPECT_TRUE(sm.rewind_files_to_checkpoint("before").ok());
    EXPECT_EQ(read_file(file), "original\n");
    sm.finalize();
    fs::remove_all(project);
    fs::remove_all(cwd);
}
