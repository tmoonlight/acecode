#include <gtest/gtest.h>

#include "daemon/platform.hpp"
#include "session/session_writer_lease.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using acecode::SessionWriterLease;
using acecode::SessionWriterLeaseResult;

namespace {

fs::path make_unique_tmp_dir(const std::string& hint) {
    auto base = fs::temp_directory_path() /
                ("acecode_writer_lease_" + hint + "_" +
                 std::to_string(::testing::UnitTest::GetInstance()
                     ->current_test_info()->line()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

bool is_acquired(const SessionWriterLeaseResult& result) {
    return result.status == SessionWriterLeaseResult::Status::Acquired;
}

} // namespace

TEST(SessionWriterLease, AcquireWritesLeaseMetadata) {
    auto dir = make_unique_tmp_dir("acquire");
    const std::string sid = "20260426-100000-abcd";

    auto result = SessionWriterLease::acquire(
        dir.string(), sid, "/tmp/work", "tui", 1234, 1000);

    ASSERT_TRUE(is_acquired(result));
    auto info = SessionWriterLease::read(dir.string(), sid);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->pid, 1234);
    EXPECT_EQ(info->session_id, sid);
    EXPECT_EQ(info->cwd, "/tmp/work");
    EXPECT_EQ(info->surface, "tui");
    EXPECT_EQ(info->updated_at_ms, 1000);
}

TEST(SessionWriterLease, SamePidCanRefreshLease) {
    auto dir = make_unique_tmp_dir("same_pid");
    const std::string sid = "20260426-100000-abcd";

    ASSERT_TRUE(is_acquired(SessionWriterLease::acquire(
        dir.string(), sid, "/tmp/work", "tui", 1234, 1000)));

    EXPECT_TRUE(SessionWriterLease::refresh(dir.string(), sid, 1234, 2000));
    auto info = SessionWriterLease::read(dir.string(), sid);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->updated_at_ms, 2000);
}

TEST(SessionWriterLease, LiveFreshOtherPidConflicts) {
    auto dir = make_unique_tmp_dir("conflict");
    const std::string sid = "20260426-100000-abcd";
    const auto current_pid = acecode::daemon::current_pid();

    ASSERT_TRUE(is_acquired(SessionWriterLease::acquire(
        dir.string(), sid, "/tmp/work", "daemon", current_pid, 1000)));

    auto result = SessionWriterLease::acquire(
        dir.string(), sid, "/tmp/work", "tui", current_pid + 1000000, 2000);

    EXPECT_EQ(result.status, SessionWriterLeaseResult::Status::Conflict);
    EXPECT_EQ(result.owner.pid, current_pid);
    EXPECT_EQ(result.owner.surface, "daemon");
}

TEST(SessionWriterLease, StaleLeaseCanBeRecovered) {
    auto dir = make_unique_tmp_dir("stale");
    const std::string sid = "20260426-100000-abcd";
    const auto current_pid = acecode::daemon::current_pid();

    ASSERT_TRUE(is_acquired(SessionWriterLease::acquire(
        dir.string(), sid, "/tmp/work", "daemon", current_pid, 1000, 5000)));

    auto result = SessionWriterLease::acquire(
        dir.string(), sid, "/tmp/work", "tui", current_pid + 1000000, 10000, 5000);

    ASSERT_TRUE(is_acquired(result));
    EXPECT_TRUE(result.stale_recovered);
    auto info = SessionWriterLease::read(dir.string(), sid);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->surface, "tui");
}

TEST(SessionWriterLease, ReleaseOnlyRemovesOwnLease) {
    auto dir = make_unique_tmp_dir("release");
    const std::string sid = "20260426-100000-abcd";

    ASSERT_TRUE(is_acquired(SessionWriterLease::acquire(
        dir.string(), sid, "/tmp/work", "tui", 1234, 1000)));

    EXPECT_FALSE(SessionWriterLease::release(dir.string(), sid, 5678));
    EXPECT_TRUE(SessionWriterLease::read(dir.string(), sid).has_value());

    EXPECT_TRUE(SessionWriterLease::release(dir.string(), sid, 1234));
    EXPECT_FALSE(SessionWriterLease::read(dir.string(), sid).has_value());
}
