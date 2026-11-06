#include <gtest/gtest.h>

#include "loop/loop_store.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <random>
#include <thread>

namespace fs = std::filesystem;
using namespace acecode::loop;

namespace {

fs::path temp_database(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_loop_store_" + hint + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir / "scheduled-loops.sqlite3";
}

bool create_legacy_database(const fs::path& path) {
    sqlite3* db = nullptr;
    const std::string path_text = path.string();
    if (sqlite3_open(path_text.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    const char* sql =
        "CREATE TABLE loops("
        "id TEXT PRIMARY KEY,name TEXT NOT NULL,prompt TEXT NOT NULL,"
        "workspace_hash TEXT NOT NULL DEFAULT '',workspace_cwd TEXT NOT NULL DEFAULT '',"
        "model_name TEXT NOT NULL,permission_mode TEXT NOT NULL,"
        "schedule_json TEXT NOT NULL,schedule_expr TEXT NOT NULL,"
        "next_run_at_ms INTEGER NULL,enabled INTEGER NOT NULL DEFAULT 1,"
        "created_at_ms INTEGER NOT NULL,updated_at_ms INTEGER NOT NULL);"
        "INSERT INTO loops(id,name,prompt,workspace_hash,workspace_cwd,model_name,"
        "permission_mode,schedule_json,schedule_expr,next_run_at_ms,enabled,"
        "created_at_ms,updated_at_ms) VALUES("
        "'legacy','Legacy','Inspect','hash','C:/workspace/hash','model-a','yolo',"
        "'{\"kind\":\"once\",\"once_at_ms\":10000}',"
        "'once:10000',10000,1,1,1);";
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return rc == SQLITE_OK;
}

LoopDefinition once_loop(const std::string& name,
                         std::int64_t when,
                         const std::string& workspace = {}) {
    LoopDefinition value;
    value.name = name;
    value.prompt = "Inspect and finish the requested code work.";
    value.model_name = "model-a";
    value.permission_mode = "yolo";
    if (!workspace.empty()) {
        value.workspace_hash = workspace;
        value.workspace_cwd = "C:/workspace/" + workspace;
    }
    value.schedule.kind = ScheduleKind::Once;
    value.schedule.once_at_ms = when;
    return value;
}

LoopDefinition daily_loop(const std::string& name,
                          int hour,
                          const std::string& workspace) {
    auto value = once_loop(name, 1);
    value.workspace_hash = workspace;
    value.workspace_cwd = "C:/workspace/" + workspace;
    value.schedule.kind = ScheduleKind::Period;
    value.schedule.period = PeriodKind::Daily;
    value.schedule.hour = hour;
    value.schedule.minute = 0;
    value.schedule.timezone_offset_minutes = 0;
    return value;
}

} // namespace

TEST(LoopStore, InitializesCrudAndEnableState) {
    const auto path = temp_database("crud");
    LoopStore store(path);
    StoreError error;
    ASSERT_TRUE(store.initialize(&error)) << error.message;
    EXPECT_TRUE(fs::exists(path));

    auto created = store.create_loop(once_loop("one", 10'000), 1'000, &error);
    ASSERT_TRUE(created.has_value()) << error.message;
    EXPECT_FALSE(created->id.empty());
    EXPECT_EQ(created->next_run_at_ms, 10'000);
    EXPECT_FALSE(created->use_worktree);

    auto listed = store.list_loops(&error);
    ASSERT_EQ(listed.size(), 1u) << error.message;
    EXPECT_EQ(listed[0].name, "one");

    auto edit = *created;
    edit.name = "renamed";
    edit.prompt = "updated prompt";
    edit.use_worktree = true;
    auto updated = store.update_loop(created->id, edit, 2'000, &error);
    ASSERT_TRUE(updated.has_value()) << error.message;
    EXPECT_EQ(updated->name, "renamed");
    EXPECT_TRUE(updated->use_worktree);
    EXPECT_EQ(updated->created_at_ms, created->created_at_ms);

    auto disabled = store.set_loop_enabled(created->id, false, 3'000, &error);
    ASSERT_TRUE(disabled.has_value()) << error.message;
    EXPECT_FALSE(disabled->enabled);
    EXPECT_FALSE(disabled->next_run_at_ms.has_value());

    ASSERT_TRUE(store.delete_loop(created->id, &error)) << error.message;
    EXPECT_TRUE(store.list_loops(&error).empty());
}

TEST(LoopStore, ManualRunPreservesDisabledAndExpiredSchedule) {
    LoopStore store(temp_database("manual"));
    StoreError error;
    ASSERT_TRUE(store.initialize(&error));
    auto loop = store.create_loop(once_loop("manual", 10'000), 1'000, &error);
    ASSERT_TRUE(loop);
    auto claimed = store.claim_now(loop->id, 2'000, "owner", &error);
    ASSERT_EQ(claimed.disposition, ClaimDisposition::Claimed) << error.message;
    EXPECT_EQ(store.get_loop(loop->id)->next_run_at_ms, loop->next_run_at_ms);
    ASSERT_TRUE(store.update_run_state(claimed.run->id, RunStatus::Completed, 3'000));
    auto disabled = store.set_loop_enabled(loop->id, false, 4'000);
    ASSERT_TRUE(disabled);
    auto manual = store.claim_now(loop->id, 20'000, "owner", &error);
    ASSERT_TRUE(manual.run) << error.message;
    EXPECT_EQ(manual.run->scheduled_at_ms, 20'000);
    EXPECT_FALSE(store.get_loop(loop->id)->enabled);
    EXPECT_EQ(store.get_loop(loop->id)->next_run_at_ms, disabled->next_run_at_ms);
    EXPECT_EQ(store.get_loop(loop->id)->updated_at_ms, disabled->updated_at_ms);
    EXPECT_EQ(store.claim_now(loop->id, 20'001, "owner", &error).disposition, ClaimDisposition::None);
    EXPECT_EQ(error.code, "LOOP_BUSY");
    error = {};
    EXPECT_FALSE(store.claim_now("missing", 20'002, "owner", &error).run);
    EXPECT_EQ(error.code, "NOT_FOUND");
}

TEST(LoopStore, ConcurrentManualClaimsAndAutomaticRunShareExclusion) {
    const auto path = temp_database("manual_race");
    LoopStore store(path), other(path);
    ASSERT_TRUE(store.initialize());
    ASSERT_TRUE(other.initialize());
    auto loop = store.create_loop(once_loop("manual", 10'000), 1'000);
    ASSERT_TRUE(loop);
    ClaimResult first, second;
    std::thread a([&] { first = store.claim_now(loop->id, 2'000, "first"); });
    std::thread b([&] { second = other.claim_now(loop->id, 2'000, "second"); });
    a.join(); b.join();
    EXPECT_NE(first.run.has_value(), second.run.has_value());
    EXPECT_EQ(store.list_runs(loop->id).size(), 1u);
    EXPECT_EQ(store.claim_due(10'000, "automatic").disposition, ClaimDisposition::MissedWorkspaceBusy);
}

TEST(LoopStore, ManualRunRejectsBusyWorkspaceAndRecoversAfterShutdown) {
    LoopStore store(temp_database("manual_workspace"));
    ASSERT_TRUE(store.initialize());
    auto a = store.create_loop(once_loop("one", 60'000, "workspace"), 1'000);
    auto b = store.create_loop(once_loop("two", 180'000, "workspace"), 1'000);
    ASSERT_TRUE(a); ASSERT_TRUE(b);
    auto claimed = store.claim_now(a->id, 2'000, "owner");
    ASSERT_TRUE(claimed.run);
    ASSERT_TRUE(store.update_run_state(claimed.run->id, RunStatus::WaitingUser, 2'001));
    StoreError error;
    EXPECT_FALSE(store.claim_now(b->id, 3'000, "owner", &error).run);
    EXPECT_EQ(error.code, "WORKSPACE_BUSY");
    EXPECT_TRUE(store.list_runs(b->id).empty());
    ASSERT_TRUE(store.interrupt_owner_runs("owner", 4'000));
    EXPECT_EQ(store.list_runs(a->id).front().status, RunStatus::Failed);
    EXPECT_TRUE(store.claim_now(b->id, 5'000, "new-owner").run);
}

TEST(LoopStore, MigratesLegacyLoopsToWorktreeWithoutChangingNewDefault) {
    const auto path = temp_database("legacy_worktree");
    ASSERT_TRUE(create_legacy_database(path));

    LoopStore store(path);
    StoreError error;
    ASSERT_TRUE(store.initialize(&error)) << error.message;

    auto legacy = store.get_loop("legacy", &error);
    ASSERT_TRUE(legacy.has_value()) << error.message;
    EXPECT_TRUE(legacy->use_worktree);

    auto created = store.create_loop(once_loop("new", 20'000), 1'000, &error);
    ASSERT_TRUE(created.has_value()) << error.message;
    EXPECT_FALSE(created->use_worktree);
    auto reloaded = store.get_loop(created->id, &error);
    ASSERT_TRUE(reloaded.has_value()) << error.message;
    EXPECT_FALSE(reloaded->use_worktree);
}

TEST(LoopStore, ConflictUpdateRollsBack) {
    LoopStore store(temp_database("conflict"));
    ASSERT_TRUE(store.initialize());
    StoreError error;
    auto first = store.create_loop(daily_loop("first", 9, "same"), 0, &error);
    auto second = store.create_loop(daily_loop("second", 10, "same"), 0, &error);
    ASSERT_TRUE(first.has_value()) << error.message;
    ASSERT_TRUE(second.has_value()) << error.message;

    auto conflicting = *second;
    conflicting.schedule.hour = 9;
    EXPECT_FALSE(store.update_loop(second->id, conflicting, 0, &error).has_value());
    EXPECT_EQ(error.code, "SCHEDULE_CONFLICT");
    ASSERT_TRUE(error.conflict.has_value());
    EXPECT_EQ(error.conflict->loop_id, first->id);

    error = {};
    auto unchanged = store.get_loop(second->id, &error);
    ASSERT_TRUE(unchanged.has_value()) << error.message;
    EXPECT_EQ(unchanged->schedule.hour, 10);
}

TEST(LoopStore, TwoConnectionsClaimOccurrenceOnlyOnce) {
    const auto path = temp_database("claim");
    LoopStore first(path);
    LoopStore second(path);
    ASSERT_TRUE(first.initialize());
    ASSERT_TRUE(second.initialize());
    auto loop = first.create_loop(once_loop("once", 10'000), 1'000);
    ASSERT_TRUE(loop.has_value());

    StoreError error;
    auto claim_a = first.claim_due(10'000, "owner-a", &error);
    ASSERT_EQ(claim_a.disposition, ClaimDisposition::Claimed) << error.message;
    auto claim_b = second.claim_due(10'000, "owner-b", &error);
    EXPECT_EQ(claim_b.disposition, ClaimDisposition::None);

    auto runs = first.list_runs(loop->id, 10, &error);
    ASSERT_EQ(runs.size(), 1u) << error.message;
    EXPECT_EQ(runs[0].owner_id, "owner-a");
    EXPECT_EQ(runs[0].status, RunStatus::Running);
}

TEST(LoopStore, WorkspaceBusyOccurrenceIsMissed) {
    LoopStore store(temp_database("busy"));
    ASSERT_TRUE(store.initialize());
    auto first = store.create_loop(once_loop("first", 10'000, "workspace"), 0);
    auto second = store.create_loop(once_loop("second", 70'000, "workspace"), 0);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());

    EXPECT_EQ(store.claim_due(10'000, "owner").disposition, ClaimDisposition::Claimed);
    auto busy = store.claim_due(70'000, "owner");
    ASSERT_EQ(busy.disposition, ClaimDisposition::MissedWorkspaceBusy);
    ASSERT_TRUE(busy.run.has_value());
    EXPECT_EQ(busy.run->reason, "workspace_busy");
    EXPECT_EQ(busy.run->status, RunStatus::Missed);
}

TEST(LoopStore, AggregatesOfflineMissedAndAdvances) {
    LoopStore store(temp_database("offline"));
    ASSERT_TRUE(store.initialize());
    LoopDefinition value = once_loop("interval", 1);
    value.schedule.kind = ScheduleKind::Interval;
    value.schedule.interval_value = 30;
    value.schedule.interval_unit = IntervalUnit::Minutes;
    value.schedule.anchor_ms = 1'000;
    auto created = store.create_loop(value, 0);
    ASSERT_TRUE(created.has_value());

    const auto now = 1'000 + 95 * kLoopMinuteMs;
    StoreError error;
    ASSERT_TRUE(store.record_offline_missed(now, "owner", &error)) << error.message;
    auto runs = store.list_runs(created->id, 10, &error);
    ASSERT_EQ(runs.size(), 1u) << error.message;
    EXPECT_EQ(runs[0].status, RunStatus::Missed);
    EXPECT_EQ(runs[0].reason, "daemon_offline");
    EXPECT_EQ(runs[0].missed_count, 4);
    auto updated = store.get_loop(created->id, &error);
    ASSERT_TRUE(updated.has_value());
    EXPECT_EQ(updated->next_run_at_ms, 1'000 + 120 * kLoopMinuteMs);
}

TEST(LoopStore, UpdatesRunLifecycleAndInterruptsOwner) {
    LoopStore store(temp_database("lifecycle"));
    ASSERT_TRUE(store.initialize());
    auto loop = store.create_loop(once_loop("once", 10'000), 0);
    ASSERT_TRUE(loop.has_value());
    auto claim = store.claim_due(10'000, "owner-a");
    ASSERT_TRUE(claim.run.has_value());

    StoreError error;
    ASSERT_TRUE(store.update_run_state(
        claim.run->id, RunStatus::WaitingUser, 11'000, {}, "session-1", "wt", "branch", &error))
        << error.message;
    auto runs = store.list_runs(loop->id, 10, &error);
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].status, RunStatus::WaitingUser);
    EXPECT_EQ(runs[0].session_id, "session-1");
    EXPECT_FALSE(runs[0].finished_at_ms.has_value());

    ASSERT_TRUE(store.interrupt_owner_runs("owner-a", 12'000, &error)) << error.message;
    runs = store.list_runs(loop->id, 10, &error);
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].status, RunStatus::Failed);
    EXPECT_EQ(runs[0].reason, "daemon_interrupted");
    EXPECT_EQ(runs[0].finished_at_ms, 12'000);
}

TEST(LoopStore, NoWorkspaceLoopsMayShareTheSameSchedule) {
    LoopStore store(temp_database("no_workspace"));
    ASSERT_TRUE(store.initialize());
    StoreError error;
    ASSERT_TRUE(store.create_loop(daily_loop("with", 9, "workspace"), 0, &error).has_value())
        << error.message;
    auto first = daily_loop("first", 9, "");
    first.workspace_hash.clear();
    first.workspace_cwd.clear();
    auto second = first;
    second.name = "second";
    ASSERT_TRUE(store.create_loop(first, 0, &error).has_value()) << error.message;
    ASSERT_TRUE(store.create_loop(second, 0, &error).has_value()) << error.message;
}

// 场景:run 结束后记录写边界事后检测的结果(主 checkout 里新出现的路径),再从
// 运行历史读回;库是老 schema(v2,loop_runs 没有 workspace_touched 列)升上来的。
// 期望:initialize 补列(v3 迁移),路径列表原样往返(换行分隔存储,读回按行
// 拆开);未知 run 报 NOT_FOUND。
TEST(LoopStore, WorkspaceTouchedRoundtripAfterMigration) {
    const auto path = temp_database("touched");
    ASSERT_TRUE(create_legacy_database(path));
    LoopStore store(path);
    StoreError error;
    ASSERT_TRUE(store.initialize(&error)) << error.message;
    auto loop = store.create_loop(once_loop("once", 10'000), 1'000, &error);
    ASSERT_TRUE(loop.has_value()) << error.message;
    auto claim = store.claim_due(10'000, "owner", &error);
    ASSERT_EQ(claim.disposition, ClaimDisposition::Claimed) << error.message;
    ASSERT_TRUE(claim.run.has_value());

    ASSERT_TRUE(store.set_run_workspace_touched(
        claim.run->id, {"electron/src/im/chat.js", "docs/notes.md"}, &error))
        << error.message;
    auto runs = store.list_runs(loop->id, 10, &error);
    ASSERT_EQ(runs.size(), 1u) << error.message;
    EXPECT_EQ(runs[0].workspace_touched,
              (std::vector<std::string>{"electron/src/im/chat.js", "docs/notes.md"}));

    error = {};
    EXPECT_FALSE(store.set_run_workspace_touched("missing-run", {"a"}, &error));
    EXPECT_EQ(error.code, "NOT_FOUND");
}

// 场景:老库(没有旧通知清理三列)升上来后读老任务、再新建 / 更新带清理开关的任务。
// 期望:initialize 补列(v4),老任务读出默认值(都关、保留 3 个);新任务的开关
// 和个数写进去能原样读回,update / set_loop_enabled 也不会把它们冲掉;存储层同样
// 拒绝两个开关同时打开,手改 REST 请求绕不过去。
TEST(LoopStore, SessionCleanupColumnsMigrateAndRoundTrip) {
    const auto path = temp_database("session_cleanup");
    ASSERT_TRUE(create_legacy_database(path));
    LoopStore store(path);
    StoreError error;
    ASSERT_TRUE(store.initialize(&error)) << error.message;

    auto legacy = store.get_loop("legacy", &error);
    ASSERT_TRUE(legacy.has_value()) << error.message;
    EXPECT_FALSE(legacy->auto_archive_old_sessions);
    EXPECT_FALSE(legacy->auto_delete_old_sessions);
    EXPECT_EQ(legacy->keep_recent_sessions, 3);

    auto value = once_loop("cleanup", 20'000);
    value.auto_archive_old_sessions = true;
    value.keep_recent_sessions = 5;
    auto created = store.create_loop(value, 1'000, &error);
    ASSERT_TRUE(created.has_value()) << error.message;
    auto reloaded = store.get_loop(created->id, &error);
    ASSERT_TRUE(reloaded.has_value()) << error.message;
    EXPECT_TRUE(reloaded->auto_archive_old_sessions);
    EXPECT_FALSE(reloaded->auto_delete_old_sessions);
    EXPECT_EQ(reloaded->keep_recent_sessions, 5);

    reloaded->auto_archive_old_sessions = false;
    reloaded->auto_delete_old_sessions = true;
    reloaded->keep_recent_sessions = 1;
    ASSERT_TRUE(store.update_loop(created->id, *reloaded, 2'000, &error)) << error.message;
    ASSERT_TRUE(store.set_loop_enabled(created->id, false, 3'000, &error)) << error.message;
    const auto updated = store.get_loop(created->id, &error);
    ASSERT_TRUE(updated.has_value()) << error.message;
    EXPECT_FALSE(updated->auto_archive_old_sessions);
    EXPECT_TRUE(updated->auto_delete_old_sessions);
    EXPECT_EQ(updated->keep_recent_sessions, 1);
    EXPECT_FALSE(updated->enabled);

    auto conflicting = *updated;
    conflicting.auto_archive_old_sessions = true;
    EXPECT_FALSE(store.update_loop(created->id, conflicting, 4'000, &error));
    EXPECT_EQ(error.code, "CONFLICTING_SESSION_CLEANUP");
}

// 场景:同一任务有四次运行:一次没建会话(空 session_id),一个会话 id 被前后两条
// 运行记录引用(异常数据);另一个任务也有会话。
// 期望:list_run_sessions 只返回本任务的非空会话 id,按计划时间从新到旧、去重,
// 重复的 id 留在它最新出现的位置。
TEST(LoopStore, ListRunSessionIdsIsNewestFirstAndDeduplicated) {
    LoopStore store(temp_database("run_sessions"));
    ASSERT_TRUE(store.initialize());
    StoreError error;
    auto loop = store.create_loop(once_loop("first", 50'000), 0, &error);
    ASSERT_TRUE(loop.has_value()) << error.message;
    auto other = store.create_loop(once_loop("other", 60'000), 0, &error);
    ASSERT_TRUE(other.has_value()) << error.message;

    const struct { std::string loop_id; std::int64_t at; std::string session; } runs[] = {
        {loop->id, 1'000, "session-a"},
        {loop->id, 2'000, ""},
        {loop->id, 3'000, "session-b"},
        {loop->id, 4'000, "session-a"},
        {other->id, 5'000, "session-other"},
    };
    for (const auto& run : runs) {
        auto claim = store.claim_now(run.loop_id, run.at, "owner", &error);
        ASSERT_EQ(claim.disposition, ClaimDisposition::Claimed) << error.message;
        ASSERT_TRUE(store.update_run_state(claim.run->id, RunStatus::Completed, run.at + 1,
                                           {}, run.session, {}, {}, &error)) << error.message;
    }

    auto moved = *loop;
    moved.workspace_cwd = "C:/next-workspace";
    moved.workspace_hash = "next-workspace";
    ASSERT_TRUE(store.update_loop(loop->id, moved, 6'000, &error));
    const auto sessions = store.list_run_sessions(loop->id, &error);
    ASSERT_EQ(sessions.size(), 2u);
    EXPECT_EQ(sessions[0].id, "session-a");
    EXPECT_EQ(sessions[1].id, "session-b");
    EXPECT_EQ(sessions[0].workspace_cwd, loop->workspace_cwd);
    EXPECT_TRUE(error.code.empty()) << error.message;
    const auto others = store.list_run_sessions(other->id, &error);
    ASSERT_EQ(others.size(), 1u);
    EXPECT_EQ(others[0].id, "session-other");
}
