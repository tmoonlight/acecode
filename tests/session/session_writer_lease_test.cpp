#include <gtest/gtest.h>

#include "platform/process/os_process.hpp"
#include "session/session_writer_lease.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
#include <type_traits>

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

static_assert(!std::is_copy_constructible_v<acecode::WriterLease>);
static_assert(std::is_nothrow_move_constructible_v<acecode::WriterLease>);

TEST(SessionWriterLease, OwnerMoveAndExceptionReleaseOnlyAtFinalScope) {
    // 场景:会话写者租约已获取,转移到新所有者后发生异常。期望移动源
    // 不删除租约,最终栈展开自动删除;原 SessionManager 的 bool 无法自动收尾。
    const auto dir = make_unique_tmp_dir("raii_move");
    const std::string sid = "raii-session";
    try {
        acecode::WriterLease original(dir.string(), sid);
        ASSERT_TRUE(is_acquired(original.acquire(dir.string(), "tui")));
        {
            acecode::WriterLease owner(std::move(original));
            original.reset();
            EXPECT_TRUE(SessionWriterLease::read(dir.string(), sid));
            EXPECT_TRUE(owner.refresh());
            throw std::runtime_error("session setup failed");
        }
    } catch (const std::runtime_error&) {}
    EXPECT_FALSE(SessionWriterLease::read(dir.string(), sid));
}

TEST(SessionWriterLease, ReacquisitionKeepsOwnerAndMoveAssignmentReleasesPrevious) {
    // 场景:同一会话重复 resume,然后移动赋值替换另一个租约。期望续用租约
    // 不被旧包装误删,被替换的租约立即释放;原 bool 不表达这些所有权关系。
    const auto dir = make_unique_tmp_dir("raii_reacquire");
    acecode::WriterLease first(dir.string(), "first"), second(dir.string(), "second");
    ASSERT_TRUE(is_acquired(first.acquire(dir.string(), "tui")));
    ASSERT_TRUE(is_acquired(first.acquire(dir.string(), "daemon")));
    ASSERT_TRUE(is_acquired(second.acquire(dir.string(), "tui")));
    second = std::move(first);
    EXPECT_FALSE(SessionWriterLease::read(dir.string(), "second"));
    ASSERT_TRUE(SessionWriterLease::read(dir.string(), "first"));
    EXPECT_EQ(SessionWriterLease::read(dir.string(), "first")->surface, "daemon");
    second.reset();
    EXPECT_FALSE(SessionWriterLease::read(dir.string(), "first"));
}

TEST(SessionWriterLease, OwnerDoesNotRemoveReplacementFromAnotherProcess) {
    // 场景:外部进程接管了租约后旧包装析构。期望保留新 PID 的租约;
    // 此用例锁定原有身份核对语义,避免 RAII 化变成无条件删除文件。
    const auto dir = make_unique_tmp_dir("raii_replacement");
    const std::string sid = "replaced";
    const auto other_pid = acecode::daemon::current_pid() + 1000000;
    {
        acecode::WriterLease lease(dir.string(), sid);
        ASSERT_TRUE(is_acquired(lease.acquire(dir.string(), "tui")));
        SessionWriterLease::remove(dir.string(), sid);
        ASSERT_TRUE(is_acquired(SessionWriterLease::acquire(
            dir.string(), sid, dir.string(), "daemon", other_pid)));
    }
    const auto replacement = SessionWriterLease::read(dir.string(), sid);
    ASSERT_TRUE(replacement);
    EXPECT_EQ(replacement->pid, other_pid);
    SessionWriterLease::remove(dir.string(), sid);
}
