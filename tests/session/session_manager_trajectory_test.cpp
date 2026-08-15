#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/session_trajectory.hpp"
#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

class SessionManagerTrajectoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        cwd_ = fs::temp_directory_path() /
            fs::path("acecode-manager-trajectory-" +
                     std::to_string(std::chrono::steady_clock::now()
                         .time_since_epoch().count()));
        fs::create_directories(cwd_);
        project_dir_ = acecode::SessionStorage::get_project_dir(
            acecode::path_to_utf8(cwd_));
        std::error_code ec;
        fs::remove_all(acecode::path_from_utf8(project_dir_), ec);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(cwd_, ec);
        fs::remove_all(acecode::path_from_utf8(project_dir_), ec);
    }

    void persist_user_message(acecode::SessionManager& manager) {
        acecode::ChatMessage message;
        message.role = "user";
        message.content = "trace this turn";
        manager.on_message(message);
    }

    fs::path cwd_;
    std::string project_dir_;
};

TEST_F(SessionManagerTrajectoryTest, ResumedSessionContinuesMonotonicSequence) {
    std::string session_id;
    std::string trajectory_path;
    {
        acecode::SessionManager manager;
        manager.start_session(
            acecode::path_to_utf8(cwd_), "test", "model");
        persist_user_message(manager);
        session_id = manager.current_session_id();
        ASSERT_FALSE(session_id.empty());
        ASSERT_TRUE(manager.record_trajectory_event(
            "message", {{"id", "first"}}, 1001));
        trajectory_path = manager.current_trajectory_path();
        manager.finalize();
    }

    {
        acecode::SessionManager resumed;
        resumed.start_session(
            acecode::path_to_utf8(cwd_), "test", "model");
        const auto messages = resumed.resume_session(session_id);
        ASSERT_FALSE(messages.empty());
        ASSERT_TRUE(resumed.record_trajectory_event(
            "message", {{"id", "second"}}, 1002));
        resumed.finalize();
    }

    const auto page = acecode::SessionTrajectoryStorage::load_page(
        trajectory_path);
    ASSERT_EQ(page.records.size(), 2u);
    EXPECT_EQ(page.records[0].sequence, 1u);
    EXPECT_EQ(page.records[1].sequence, 2u);
    EXPECT_EQ(page.records[1].payload.value("id", std::string{}), "second");
}

TEST_F(SessionManagerTrajectoryTest, PurgeRemovesTrajectorySidecar) {
    acecode::SessionManager manager;
    manager.start_session(
        acecode::path_to_utf8(cwd_), "test", "model");
    persist_user_message(manager);
    const std::string session_id = manager.current_session_id();
    ASSERT_TRUE(manager.record_trajectory_event("turn_end", {{"outcome", "completed"}}));
    const std::string trajectory_path = manager.current_trajectory_path();
    manager.finalize();
    ASSERT_TRUE(fs::is_regular_file(acecode::path_from_utf8(trajectory_path)));

    std::string error;
    EXPECT_TRUE(acecode::SessionStorage::purge_session_files(
        project_dir_, session_id, &error)) << error;
    EXPECT_FALSE(fs::exists(acecode::path_from_utf8(trajectory_path)));
}

} // namespace
