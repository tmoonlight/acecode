#include <gtest/gtest.h>
#include "session/session_recent_activity.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/global_session_search.hpp"
#include "session/turn_timing.hpp"
#include "utils/cwd_hash.hpp"
#include "utils/uuid.hpp"
#include "test_support/memory/memory_test_home.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace acecode;
namespace fs = std::filesystem;
namespace {
class RecentFixture : public testing::Test {
protected:
    acecode_test::MemoryTestHome home{"office-recency"};
    fs::path root = home.root() / "catalog";
    void SetUp() override { fs::create_directories(root); }
    void TearDown() override { std::error_code ec; fs::remove_all(root, ec); }
};
ChatMessage input(const char* time, const char* text = "hello") {
    ChatMessage result; result.role = "user"; result.content = text;
    result.timestamp = time; result.uuid = generate_uuid(); return result;
}
}

TEST_F(RecentFixture, ReverseReadIgnoresBackgroundMessagesAndDamagedTail) {
    const auto file = (root / "session.jsonl").string();
    auto user = input("2026-10-06T09:10:00Z");
    ASSERT_TRUE(SessionStorage::append_message(file, user));
    auto internal = input("2026-10-06T09:11:00Z");
    internal.metadata = {{"hidden_goal_context", true}};
    ASSERT_TRUE(SessionStorage::append_message(file, internal));
    auto mesh = input("2026-10-06T09:12:00Z");
    mesh.metadata = {{"inter_agent", {{"type", "MESSAGE"}, {"sender", "/root/a"}, {"recipient", "/root"}}}};
    ASSERT_TRUE(SessionStorage::append_message(file, mesh));
    ASSERT_TRUE(SessionStorage::append_message(file, make_turn_timing_message(
        {user.uuid, 1, 2, 1, "completed"}, "2026-10-06T09:13:00Z")));
    std::ofstream(file, std::ios::app) << "{broken\n";
    auto automatic = input("2026-10-06T09:14:00Z");
    automatic.metadata = {{"source_tool", "tool_search"}};
    ASSERT_TRUE(SessionStorage::append_message(file, automatic));
    const auto recent = read_session_recent_activity(file);
    EXPECT_EQ(recent.last_user_message_at, user.timestamp);
    EXPECT_EQ(recent.last_turn_outcome, "completed");
    auto next = input("2026-10-06T10:00:00Z");
    ASSERT_TRUE(SessionStorage::append_message(file, next));
    EXPECT_TRUE(read_session_recent_activity(file).last_turn_outcome.empty());
}

TEST_F(RecentFixture, ManagerPersistsOnlyHumanRecencyAndKeepsItOnResume) {
    SessionManager manager;
    manager.start_session(root.string(), "test", "test");
    const auto user = input("2026-10-06T09:10:00Z");
    manager.on_message(user);
    const auto id = manager.current_session_id();
    const auto directory = manager.current_project_dir();
    const auto metaPath = SessionStorage::meta_path(directory, id);
    auto reply = user; reply.role = "assistant"; reply.timestamp = "2026-10-06T10:00:00Z";
    manager.on_message(reply);
    manager.set_session_title("renamed");
    EXPECT_EQ(SessionStorage::read_meta(metaPath).last_user_message_at, user.timestamp);
    manager.on_message(make_turn_timing_message({user.uuid,1,2,1,"aborted"},reply.timestamp));
    EXPECT_EQ(SessionStorage::read_meta(metaPath).last_turn_outcome, "aborted");
    manager.resume_session(id);
    EXPECT_EQ(manager.display_snapshot().last_user_message_at, user.timestamp);
    EXPECT_EQ(manager.display_snapshot().last_turn_outcome, "aborted");
    manager.on_message(input("2026-10-06T11:00:00Z"));
    EXPECT_EQ(SessionStorage::read_meta(metaPath).last_user_message_at, "2026-10-06T11:00:00Z");
    EXPECT_TRUE(SessionStorage::read_meta(metaPath).last_turn_outcome.empty());
    manager.finalize();
    std::error_code ec; fs::remove_all(directory, ec);
}

TEST_F(RecentFixture, GlobalFiveUseUserTimeAcrossProjectsAndExcludeChildrenAndArchive) {
    for (int i = 0; i < 8; ++i) {
        const auto directory = root / (i % 2 == 0 ? "project-a" : "project-b");
        fs::create_directories(directory);
        SessionMeta meta;
        meta.id = "20261006-10000" + std::to_string(i) + "-abcd";
        meta.cwd = directory.string(); meta.created_at = "2026-10-06T00:00:00Z";
        meta.updated_at = "2026-10-06T23:00:0" + std::to_string(9-i) + "Z";
        meta.last_user_message_at = "2026-10-06T10:00:0" + std::to_string(i) + "Z";
        if (i == 7) meta.archived = true;
        if (i == 6) meta.parent_session_id = "parent";
        // One legacy entry must derive its real input time from JSONL.
        const auto message = input(meta.last_user_message_at.c_str());
        ASSERT_TRUE(SessionStorage::append_message(SessionStorage::session_path(directory.string(),meta.id),message));
        if (i == 5) meta.last_user_message_at.clear();
        ASSERT_TRUE(SessionStorage::write_meta(SessionStorage::meta_path(directory.string(),meta.id),meta));
    }
    GlobalSessionSearchService service(root.string());
    service.start(); service.notify_startup_interaction();
    GlobalSessionSearchPage result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        result = service.recent_user_sessions();
        if (result.progress.complete) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    service.stop();
    ASSERT_TRUE(result.progress.complete);
    ASSERT_EQ(result.entries.size(),5u);
    EXPECT_EQ(result.entries.front().meta.id,"20261006-100005-abcd");
    EXPECT_EQ(result.entries.back().meta.id,"20261006-100001-abcd");
}

TEST_F(RecentFixture, ForkUsesRetainedInputTimeWithoutPretendingItWasSentNow) {
    SessionManager manager;
    manager.start_session(root.string(), "test", "test");
    const auto first = input("2026-10-06T09:10:00Z");
    const auto second = input("2026-10-06T10:00:00Z");
    manager.on_message(first); manager.on_message(second);
    const auto source = manager.current_session_id();
    const auto fork = manager.fork_session_to_new_id({first}, "branch", source, first.uuid);
    ASSERT_FALSE(fork.empty());
    const auto meta = SessionStorage::read_meta(SessionStorage::meta_path(manager.current_project_dir(), fork));
    EXPECT_EQ(meta.last_user_message_at, first.timestamp);
    EXPECT_EQ(manager.current_session_id(), source);
    EXPECT_EQ(manager.display_snapshot().last_user_message_at, second.timestamp);
    const auto active_fork = manager.fork_active_session({first});
    ASSERT_FALSE(active_fork.empty());
    EXPECT_EQ(manager.display_snapshot().last_user_message_at, first.timestamp);
}

TEST_F(RecentFixture, InternalTurnOutcomePersistsWithoutChangingHumanRecency) {
    SessionManager manager;
    manager.start_session(root.string(), "test", "test");
    auto internal = input("2026-10-06T10:00:00Z");
    internal.metadata = {{"inter_agent", {{"type", "MESSAGE"}, {"sender", "/root"}, {"recipient", "/root/a"}}}};
    manager.on_message(internal);
    manager.record_turn_outcome("completed");
    EXPECT_TRUE(manager.display_snapshot().last_user_message_at.empty());
    EXPECT_EQ(manager.display_snapshot().last_turn_outcome, "completed");
    const auto id = manager.current_session_id();
    manager.resume_session(id);
    EXPECT_EQ(manager.display_snapshot().last_turn_outcome, "completed");
}
