// 覆盖 src/environment/data_dir_migration.{hpp,cpp}:目标校验、排除规则、复制 + 指针、
// 后台任务壳、旧目录清理与清理提示阈值。全部在临时目录里跑,HOME / USERPROFILE 指向
// 临时目录以控制"平台默认目录"。

#include <gtest/gtest.h>

#include "environment/data_dir_migration.hpp"
#include "utils/encoding.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"
#include "utils/state_file.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sqlite3.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace acecode::environment;

namespace {

#ifdef _WIN32
constexpr const char* kHomeEnv = "USERPROFILE";
#else
constexpr const char* kHomeEnv = "HOME";
#endif

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    ofs << content;
}

class DataDirMigrationTest : public ::testing::Test {
protected:
    fs::path root;         // 临时根
    fs::path home;         // HOME
    fs::path default_dir;  // <home>/.acecode(平台默认目录 = 当前数据目录)
    std::string prev_home;
    bool had_home = false;

    void SetUp() override {
        acecode::reset_run_mode_for_test();
        if (const char* e = std::getenv(kHomeEnv)) { prev_home = e; had_home = true; }
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("acecode-migration-" + std::to_string(now));
        home = root / "home";
        default_dir = home / ".acecode";
        fs::create_directories(default_dir);
        set_env(kHomeEnv, home.string());
        acecode::reset_data_dir_cache_for_test();
    }
    void TearDown() override {
        reset_data_dir_write_gate_for_test();
        if (had_home) set_env(kHomeEnv, prev_home);
        acecode::reset_run_mode_for_test();
        std::error_code ec;
        // 走扩展长度路径:超长路径用例留下的深层文件普通路径删不掉,会残留在 %TEMP%。
        fs::remove_all(acecode::to_extended_length_path(root), ec);
    }

    // 造一棵典型的数据目录:配置、会话、运行时文件、锁文件、sqlite 三件套。
    void populate_source(const fs::path& dir) {
        write_file(dir / "config.json", R"({"provider":""})");
        write_file(dir / "state.json", "{}");
        write_file(dir / "config.json.lock", "");
        write_file(dir / "projects" / "abc" / "sessions" / "s1.jsonl", "line1\nline2\n");
        write_file(dir / "projects" / "abc" / "workspace.json", "{}");
        write_file(dir / "memory" / "MEMORY.md", "# memory");
        write_file(dir / "run" / "daemon.pid", "123");
        write_file(dir / "tmp" / "scratch.txt", "junk");
        write_file(dir / "scheduled-loops.sqlite3", "db");
        write_file(dir / "scheduled-loops.sqlite3-wal", "wal");
        write_file(dir / "scheduled-loops.sqlite3-shm", "shm");
    }

    std::string s(const fs::path& p) const { return acecode::path_to_utf8(p); }
};

}  // namespace

TEST_F(DataDirMigrationTest, InvalidTargetsNeverDeleteExistingData) {
    populate_source(default_dir);
    for (const auto& target : {default_dir, home, root / "occupied"}) {
        write_file(target / "keep.txt", "keep");
        const auto result = run_data_dir_migration(s(default_dir), s(default_dir), s(target));
        EXPECT_EQ(result.state, "failed");
        EXPECT_TRUE(fs::exists(target / "keep.txt"));
        EXPECT_TRUE(fs::exists(default_dir / "config.json"));
    }
}

TEST_F(DataDirMigrationTest, ChangingSourceDoesNotPublishPointer) {
    populate_source(default_dir);
    bool changed = false;
    const auto result = run_data_dir_migration(s(default_dir), s(default_dir), s(root / "moved"),
        [&](auto copied, auto) {
            if (copied && !changed) { changed = true; write_file(default_dir / "new-session.jsonl", "new"); }
        });
    EXPECT_EQ(result.state, "failed");
    EXPECT_FALSE(acecode::read_data_dir_redirect(s(default_dir)));
    EXPECT_TRUE(fs::exists(default_dir / "new-session.jsonl"));
    // staging 前缀取常量:写死旧前缀的话,前缀改名后这条断言会永远空过。
    for (const auto& entry : fs::directory_iterator(root)) {
        EXPECT_EQ(entry.path().filename().string().find(kMigrationStagingPrefix), std::string::npos);
    }
}

TEST_F(DataDirMigrationTest, CopiesCommittedWalDataUsingDatabaseSnapshot) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(s(default_dir / "state.sqlite3").c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "PRAGMA journal_mode=WAL; CREATE TABLE messages(body TEXT); INSERT INTO messages VALUES('saved conversation');", nullptr, nullptr, nullptr), SQLITE_OK);
    const auto target = root / "moved";
    const auto result = run_data_dir_migration(s(default_dir), s(default_dir), s(target));
    sqlite3_close(db);
    ASSERT_EQ(result.state, "done") << result.error;
    ASSERT_EQ(sqlite3_open(s(target / "state.sqlite3").c_str(), &db), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT body FROM messages", -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)), "saved conversation");
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    EXPECT_FALSE(fs::exists(target / "state.sqlite3-wal"));
}

TEST_F(DataDirMigrationTest, FailedJobReopensWritesAndResumesScheduler) {
    DataDirMigrationJob job;
    bool resumed = false;
    std::string error;
    ASSERT_TRUE(job.start(s(default_dir), s(default_dir), s(root / "moved"), &error,
        [] { EXPECT_FALSE(acecode::try_write_state_flag("late_background_write", true)); throw std::runtime_error("cannot pause writer"); }, [&] { resumed = true; }));
    job.wait_for_test();
    ASSERT_TRUE(job.progress());
    EXPECT_EQ(job.progress()->state, "failed");
    EXPECT_FALSE(data_dir_writes_blocked());
    EXPECT_TRUE(resumed);
    EXPECT_TRUE(acecode::try_write_state_flag("after_failed_migration", true));
}

// 场景:目标校验的每一种拒绝原因。
// 期望:相对路径 / 同目录 / 在当前目录里 / 包含当前目录 / 是文件 / 非空目录 各自
// 命中稳定错误码;有效的不存在目标被创建为空目录并通过。
TEST_F(DataDirMigrationTest, ValidateTargetRejectsEachInvalidShape) {
    populate_source(default_dir);
    const std::string cur = s(default_dir);

    EXPECT_EQ(validate_migration_target(cur, "relative/dir").error, MigrationTargetError::NotAbsolute);
    EXPECT_EQ(validate_migration_target(cur, cur).error, MigrationTargetError::SameAsCurrent);
    EXPECT_EQ(validate_migration_target(cur, s(default_dir / "backup")).error,
              MigrationTargetError::InsideCurrent);
    EXPECT_EQ(validate_migration_target(cur, s(home)).error, MigrationTargetError::ContainsCurrent);

    write_file(root / "afile.txt", "x");
    EXPECT_EQ(validate_migration_target(cur, s(root / "afile.txt")).error,
              MigrationTargetError::NotADirectory);

    fs::create_directories(root / "nonempty");
    write_file(root / "nonempty" / "keep.txt", "x");
    EXPECT_EQ(validate_migration_target(cur, s(root / "nonempty")).error,
              MigrationTargetError::NotEmpty);

    auto ok = validate_migration_target(cur, s(root / "fresh"));
    EXPECT_EQ(ok.error, MigrationTargetError::None) << ok.message;
    EXPECT_TRUE(fs::is_directory(root / "fresh")) << "有效目标应被创建为空目录";
    EXPECT_FALSE(fs::exists(root / "fresh" / ".acecode-write-probe")) << "探针文件必须清掉";

    EXPECT_STREQ(migration_target_error_code(MigrationTargetError::NotEmpty), "TARGET_NOT_EMPTY");
    EXPECT_STREQ(migration_target_error_code(MigrationTargetError::InsideCurrent),
                 "TARGET_INSIDE_CURRENT");
}

// 场景:大小写 / 尾部分隔符不同的同一目录(Windows 常见)。
// 期望:仍判为同一目录,不能靠改大小写绕过。
TEST_F(DataDirMigrationTest, ValidateTargetUsesCanonicalComparison) {
#ifdef _WIN32
    populate_source(default_dir);
    std::string upper = s(default_dir);
    for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    EXPECT_EQ(validate_migration_target(s(default_dir), upper + "\\").error,
              MigrationTargetError::SameAsCurrent);
#else
    GTEST_SKIP() << "case-insensitive comparison is Windows-only";
#endif
}

// 场景:排除规则。
// 期望:顶层 run/、tmp/、指针文件、任意 *.lock 被排除;projects/ 下的会话文件与
// 深层目录不排除;sqlite 主库 / wal / shm / journal 被识别为最后复制的一组。
// 另含 edge-app-profile、无工作区会话临时目录的锚定排除,尽力复制子树根的判定,
// 以及浏览器可再生缓存的跳过规则。
TEST_F(DataDirMigrationTest, ExclusionRules) {
    EXPECT_TRUE(migration_excludes_entry(fs::path("run")));
    EXPECT_TRUE(migration_excludes_entry(fs::path("run") / "daemon.pid"));
    EXPECT_TRUE(migration_excludes_entry(fs::path("tmp") / "x"));
    EXPECT_TRUE(migration_excludes_entry(fs::path("config.json.lock")));
    EXPECT_TRUE(migration_excludes_entry(fs::path("projects") / "a" / ".writer.lock"));
    EXPECT_TRUE(migration_excludes_entry(fs::path(acecode::kDataDirRedirectFileName)));
    EXPECT_TRUE(migration_excludes_entry(fs::path("data_dir.redirect.json")));
    EXPECT_FALSE(migration_excludes_entry(fs::path("projects") / "data_dir.redirect.json"));
    EXPECT_FALSE(migration_excludes_entry(fs::path("projects") / "a" / "s.jsonl"));
    EXPECT_FALSE(migration_excludes_entry(fs::path("runbook.md")));
    EXPECT_FALSE(migration_excludes_entry(fs::path("memory")));

    // edge-app-profile:webapp 兼容模式每次启动都重建的 Edge profile,整个排除。
    EXPECT_TRUE(migration_excludes_entry(fs::path("edge-app-profile")));
    EXPECT_TRUE(migration_excludes_entry(fs::path("edge-app-profile") / "u1" / "Default" / "x"));
    // 无工作区会话的 ACECODE_TMPDIR 锚定到 cache/no-workspace/<id>/.acecode/tmp 这一层;
    // 同一会话目录下 Agent 的产出文件与 .acecode 下的其它内容照常迁移。
    const fs::path nw = fs::path("cache") / "no-workspace" / "20260924-abc";
    EXPECT_TRUE(migration_excludes_entry(nw / ".acecode" / "tmp"));
    EXPECT_TRUE(migration_excludes_entry(nw / ".acecode" / "tmp" / "session-1" / "shot.png"));
    EXPECT_FALSE(migration_excludes_entry(nw / "report.md"));
    EXPECT_FALSE(migration_excludes_entry(nw / ".acecode" / "skills"));
    EXPECT_FALSE(migration_excludes_entry(nw / ".acecode"));
    EXPECT_FALSE(migration_excludes_entry(fs::path("projects") / "x" / ".acecode" / "tmp"));

    // 尽力复制子树的根恰好是 agent-browser/webview2,父目录与兄弟文件不算。
    EXPECT_TRUE(migration_is_best_effort_root(fs::path("agent-browser") / "webview2"));
    EXPECT_FALSE(migration_is_best_effort_root(fs::path("agent-browser") / "macos-profile-id"));
    EXPECT_FALSE(migration_is_best_effort_root(fs::path("agent-browser")));
    EXPECT_FALSE(migration_is_best_effort_root(fs::path("agent-browser") / "webview2" / "Default"));
    // 新装环境的目录是下划线形式 agent_browser/webview2(运行数据命名规则,只有旧的
    // 连字符目录已存在时才沿用旧名),同样是尽力复制子树的根;边界规则与连字符形式一致。
    EXPECT_TRUE(migration_is_best_effort_root(fs::path("agent_browser") / "webview2"));
    EXPECT_FALSE(migration_is_best_effort_root(fs::path("agent_browser") / "macos_profile_id"));
    EXPECT_FALSE(migration_is_best_effort_root(fs::path("agent_browser")));
    EXPECT_FALSE(migration_is_best_effort_root(fs::path("agent_browser") / "webview2" / "Default"));

    // 可再生缓存与锁文件跳过;登录态相关的库与配置不跳过。
    EXPECT_TRUE(migration_skips_rebuildable_browser_entry(fs::path("Cache")));
    EXPECT_TRUE(migration_skips_rebuildable_browser_entry(
        fs::path("agent-browser") / "webview2" / "Default" / "Code Cache" / "js" / "index"));
    EXPECT_TRUE(migration_skips_rebuildable_browser_entry(fs::path("lockfile")));
    EXPECT_TRUE(migration_skips_rebuildable_browser_entry(fs::path("Default") / "Local Storage" / "leveldb" / "LOCK"));
    EXPECT_FALSE(migration_skips_rebuildable_browser_entry(fs::path("Cookies")));
    EXPECT_FALSE(migration_skips_rebuildable_browser_entry(fs::path("Local State")));
    EXPECT_FALSE(migration_skips_rebuildable_browser_entry(fs::path("Default") / "Network" / "Cookies"));

    EXPECT_TRUE(migration_is_sqlite_family(fs::path("scheduled-loops.sqlite3")));
    EXPECT_TRUE(migration_is_sqlite_family(fs::path("scheduled-loops.sqlite3-wal")));
    EXPECT_TRUE(migration_is_sqlite_family(fs::path("x") / "state.sqlite3-shm"));
    // -journal 也归入 sqlite 组:排在主库之后复制,主库快照后才能按旁路文件跳过。
    EXPECT_TRUE(migration_is_sqlite_family(fs::path("x") / "state.sqlite3-journal"));
    EXPECT_FALSE(migration_is_sqlite_family(fs::path("config.json")));
}

// 场景:完整迁移成功。
// 期望:目标里有 config.json / projects / memory / sqlite 三件套,没有 run、tmp、
// *.lock;默认目录里写下指针(目标、旧目录、复制字节数、cleanup_pending);
// 进度 state=done 且 restart_required;copied 等于被复制文件的总大小。
TEST_F(DataDirMigrationTest, MigrationCopiesAndWritesPointer) {
    populate_source(default_dir);
    const fs::path target = root / "moved";
    unsigned long long last_total = 0;
    auto result = run_data_dir_migration(s(default_dir), s(default_dir), s(target),
        [&](unsigned long long, unsigned long long total) { last_total = total; });
    ASSERT_EQ(result.state, "done") << result.error;
    EXPECT_TRUE(result.restart_required);
    EXPECT_EQ(result.skipped_files, 0u) << "没有浏览器 profile 时不应有任何跳过";
    EXPECT_TRUE(fs::exists(target / "config.json"));
    EXPECT_TRUE(fs::exists(target / "projects" / "abc" / "sessions" / "s1.jsonl"));
    EXPECT_TRUE(fs::exists(target / "memory" / "MEMORY.md"));
    EXPECT_TRUE(fs::exists(target / "scheduled-loops.sqlite3-wal"));
    EXPECT_FALSE(fs::exists(target / "run"));
    EXPECT_FALSE(fs::exists(target / "tmp"));
    EXPECT_FALSE(fs::exists(target / "config.json.lock"));
    EXPECT_FALSE(fs::exists(target / acecode::kDataDirRedirectFileName));
    EXPECT_FALSE(fs::exists(target / "data_dir.redirect.json"));

    // 复制字节数 = 被复制文件大小之和(排除项不计)。
    unsigned long long expected = 0;
    for (const char* rel : {"config.json", "state.json", "projects/abc/sessions/s1.jsonl",
                            "projects/abc/workspace.json", "memory/MEMORY.md",
                            "scheduled-loops.sqlite3", "scheduled-loops.sqlite3-wal",
                            "scheduled-loops.sqlite3-shm"}) {
        expected += fs::file_size(default_dir / rel);
    }
    EXPECT_EQ(result.copied_bytes, expected);
    EXPECT_EQ(result.total_bytes, expected);
    EXPECT_EQ(last_total, expected);

    auto pointer = acecode::read_data_dir_redirect(s(default_dir));
    ASSERT_TRUE(pointer.has_value());
    EXPECT_TRUE(fs::equivalent(acecode::path_from_utf8(pointer->data_dir), target));
    EXPECT_TRUE(fs::equivalent(acecode::path_from_utf8(pointer->previous_data_dir), default_dir));
    // 指针记的是旧目录占用:复制量 + 未迁移但仍留在旧目录的 run/、tmp/ 与锁文件。
    unsigned long long left_behind = 0;
    for (const char* rel : {"run/daemon.pid", "tmp/scratch.txt", "config.json.lock"}) {
        left_behind += fs::file_size(default_dir / rel);
    }
    EXPECT_EQ(pointer->previous_size_bytes, expected + left_behind);
    EXPECT_TRUE(pointer->cleanup_pending);
    EXPECT_GT(pointer->migrated_at_ms, 0);

    // 指针生效:重新解析后数据目录指向 target。
    acecode::reset_data_dir_cache_for_test();
    EXPECT_TRUE(fs::equivalent(acecode::path_from_utf8(
        acecode::resolve_data_dir(acecode::RunMode::User)), target));
}

// 场景:目标不合法(在当前目录里面)。
// 期望:state=failed,错误带稳定错误码,不写指针,半成品目标被删掉。
TEST_F(DataDirMigrationTest, MigrationFailureLeavesNoPointerAndRemovesTarget) {
    populate_source(default_dir);
    const fs::path target = default_dir / "inner";
    auto result = run_data_dir_migration(s(default_dir), s(default_dir), s(target));
    EXPECT_EQ(result.state, "failed");
    EXPECT_NE(result.error.find("TARGET_INSIDE_CURRENT"), std::string::npos);
    EXPECT_FALSE(acecode::read_data_dir_redirect(s(default_dir)).has_value());
    EXPECT_FALSE(fs::exists(target));
}

// 场景:后台任务壳。
// 期望:start 后 active;跑完 progress 为 done;跑的过程中再次 start 被拒;
// 结束后可再次 start。
TEST_F(DataDirMigrationTest, JobRunsInBackgroundAndRejectsConcurrentStart) {
    populate_source(default_dir);
    DataDirMigrationJob job;
    EXPECT_FALSE(job.progress().has_value());
    std::string err;
    ASSERT_TRUE(job.start(s(default_dir), s(default_dir), s(root / "moved-a"), &err)) << err;
    // 极短的复制可能瞬间完成;只断言"若仍在跑则第二次被拒"。
    if (job.active()) {
        std::string err2;
        EXPECT_FALSE(job.start(s(default_dir), s(default_dir), s(root / "moved-b"), &err2));
        EXPECT_FALSE(err2.empty());
    }
    job.wait_for_test();
    EXPECT_FALSE(job.active());
    auto p = job.progress();
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->state, "done") << p->error;
    EXPECT_TRUE(p->restart_required);
    EXPECT_TRUE(data_dir_writes_blocked());
    EXPECT_FALSE(job.start(s(default_dir), s(default_dir), s(root / "moved-b"), &err));
}

// 场景:清理旧目录。
// 期望:旧目录 = 默认目录时只保留指针文件;旧目录是别处时整个目录删除;
// acknowledge 把 cleanup_pending 清掉。
TEST_F(DataDirMigrationTest, CleanupPreviousKeepsPointerOnlyInDefaultDir) {
    populate_source(default_dir);
    acecode::DataDirRedirect r;
    r.data_dir = s(root / "elsewhere");
    r.previous_data_dir = s(default_dir);
    r.cleanup_pending = true;
    ASSERT_TRUE(acecode::write_data_dir_redirect(s(default_dir), r));

    EXPECT_EQ(cleanup_previous_data_dir(s(default_dir), s(default_dir)), "");
    EXPECT_TRUE(fs::exists(acecode::path_from_utf8(acecode::data_dir_redirect_path(s(default_dir)))));
    EXPECT_FALSE(fs::exists(default_dir / "config.json"));
    EXPECT_FALSE(fs::exists(default_dir / "projects"));

    fs::create_directories(root / "old-elsewhere" / "sub");
    write_file(root / "old-elsewhere" / "sub" / "f.txt", "x");
    EXPECT_FALSE(cleanup_previous_data_dir(s(root / "old-elsewhere"), s(default_dir)).empty());
    EXPECT_TRUE(fs::exists(root / "old-elsewhere" / "sub" / "f.txt"));
    r.previous_data_dir = s(root / "old-elsewhere");
    ASSERT_TRUE(acecode::write_data_dir_redirect(s(default_dir), r));
    EXPECT_EQ(cleanup_previous_data_dir(s(root / "old-elsewhere"), s(default_dir)), "");
    EXPECT_FALSE(fs::exists(root / "old-elsewhere"));

    r.previous_data_dir = s(root / "never-existed");
    ASSERT_TRUE(acecode::write_data_dir_redirect(s(default_dir), r));
    EXPECT_EQ(cleanup_previous_data_dir(r.previous_data_dir, s(default_dir)), "")
        << "不存在的旧目录视为已清理";

    EXPECT_EQ(acknowledge_data_dir_cleanup(s(default_dir)), "");
    auto back = acecode::read_data_dir_redirect(s(default_dir));
    ASSERT_TRUE(back.has_value());
    EXPECT_FALSE(back->cleanup_pending);
}

TEST_F(DataDirMigrationTest, CleanupPreservesBothRedirectNamingGenerations) {
    populate_source(default_dir);
    write_file(default_dir / acecode::kDataDirRedirectFileName, "{}");
    acecode::DataDirRedirect redirect;
    redirect.data_dir = s(root / "elsewhere");
    redirect.previous_data_dir = s(default_dir);
    redirect.cleanup_pending = true;
    ASSERT_TRUE(acecode::write_data_dir_redirect(s(default_dir), redirect));
    // An older installation still owns the hyphenated pointer when both exist.
    write_file(default_dir / "data_dir.redirect.json", "{}");

    EXPECT_EQ(cleanup_previous_data_dir(s(default_dir), s(default_dir)), "");
    EXPECT_TRUE(fs::exists(default_dir / acecode::kDataDirRedirectFileName));
    EXPECT_TRUE(fs::exists(default_dir / "data_dir.redirect.json"));
    const auto retained = acecode::read_data_dir_redirect(s(default_dir));
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained->data_dir, redirect.data_dir);
    EXPECT_FALSE(data_dir_status(acecode::RunMode::User).previous_exists);
}

// 场景:状态汇总与 100 MB 阈值。
// 期望:大迁移 + pending + 旧目录仍有内容 → cleanup_prompt;小迁移 → 不提示但
// previous_dir 仍报出;acknowledge 后不再提示;没有指针 → 纯默认状态。
TEST_F(DataDirMigrationTest, StatusReportsCleanupPromptAboveThreshold) {
    auto plain = data_dir_status(acecode::RunMode::User);
    EXPECT_FALSE(plain.redirect_active);
    EXPECT_TRUE(plain.previous_dir.empty());
    EXPECT_FALSE(plain.cleanup_prompt);
    EXPECT_TRUE(fs::equivalent(acecode::path_from_utf8(plain.default_dir), default_dir));

    populate_source(default_dir);  // 旧数据留在默认目录
    const fs::path target = root / "moved";
    fs::create_directories(target);
    acecode::DataDirRedirect r;
    r.data_dir = s(target);
    r.previous_data_dir = s(default_dir);
    r.previous_size_bytes = kCleanupPromptThresholdBytes + 1;
    r.cleanup_pending = true;
    ASSERT_TRUE(acecode::write_data_dir_redirect(s(default_dir), r));
    acecode::reset_data_dir_cache_for_test();

    auto big = data_dir_status(acecode::RunMode::User);
    EXPECT_TRUE(big.redirect_active);
    EXPECT_TRUE(fs::equivalent(acecode::path_from_utf8(big.effective_dir), target));
    EXPECT_TRUE(big.previous_exists);
    EXPECT_TRUE(big.cleanup_pending);
    EXPECT_TRUE(big.cleanup_prompt);

    r.previous_size_bytes = 40ULL * 1024 * 1024;
    ASSERT_TRUE(acecode::write_data_dir_redirect(s(default_dir), r));
    auto small = data_dir_status(acecode::RunMode::User);
    EXPECT_FALSE(small.cleanup_prompt);
    EXPECT_FALSE(small.previous_dir.empty()) << "不足阈值仍要报旧目录供手动删除";

    r.previous_size_bytes = kCleanupPromptThresholdBytes + 1;
    ASSERT_TRUE(acecode::write_data_dir_redirect(s(default_dir), r));
    EXPECT_EQ(acknowledge_data_dir_cleanup(s(default_dir)), "");
    auto acked = data_dir_status(acecode::RunMode::User);
    EXPECT_FALSE(acked.cleanup_pending);
    EXPECT_FALSE(acked.cleanup_prompt);
}

namespace {

// 模拟 MSVC 的 system_category().message():中文 Windows 上它走 ANSI 代码页,返回的是
// GBK 字节。这里固定返回「系统找不到」的 GBK 编码(CF B5 CD B3 D5 D2 B2 BB B5 BD),
// 其中 D5 D2 不是合法 UTF-8 序列,整串一定是非法 UTF-8。
class GbkMessageCategory : public std::error_category {
public:
    const char* name() const noexcept override { return "gbk-test"; }
    std::string message(int) const override {
        return "\xCF\xB5\xCD\xB3\xD5\xD2\xB2\xBB\xB5\xBD";
    }
};

const GbkMessageCategory& gbk_message_category() {
    static const GbkMessageCategory category;
    return category;
}

}  // namespace

// 场景:迁移过程中某个文件系统调用失败,ec.message() 是 GBK 字节(中文 Windows 的常态,
// 0923 反馈日志原文就是 GBK 的「系统找不到指定的路径。」)。
// 期望:migration_os_error_text 的结果恒为合法 UTF-8;在 ACP=936 的机器上还要准确还原成
// 「系统找不到」,而不是被替换成 '?'。
// bug 表现:修复前这段文本原样进 progress.error,json dump 抛 type_error.316,
// /migration 与 /data-dir 每次轮询都 500,前端永远卡在「迁移中」只能重启。
TEST_F(DataDirMigrationTest, OsErrorTextIsAlwaysValidUtf8) {
    const std::error_code ec(3, gbk_message_category());
    ASSERT_FALSE(acecode::is_valid_utf8(ec.message()));  // 前提:原文确实是非法 UTF-8
    const auto text = migration_os_error_text(ec);
    EXPECT_TRUE(acecode::is_valid_utf8(text)) << text;
    EXPECT_FALSE(text.empty());
#ifdef _WIN32
    if (GetACP() == 936) {
        // 「系统找不到」的 UTF-8 字节,直接写字节避免依赖源文件编码 / char8_t。
        EXPECT_EQ(text, "\xE7\xB3\xBB\xE7\xBB\x9F\xE6\x89\xBE\xE4\xB8\x8D\xE5\x88\xB0");
    }
#endif
}

// 场景:后台迁移线程里抛出的异常 what() 带 GBK 字节(例如 before_copy 暂停写入方失败,
// 或标准库异常里拼进了 OS 错误文本)。
// 期望:任务失败(state=failed),progress.error 是合法 UTF-8,可以安全序列化成 JSON。
// bug 表现:修复前 result.error = e.what() 原样保存,同样让 /migration 永久 500。
TEST_F(DataDirMigrationTest, JobExceptionTextIsSanitizedToUtf8) {
    DataDirMigrationJob job;
    std::string error;
    ASSERT_TRUE(job.start(s(default_dir), s(default_dir), s(root / "moved"), &error,
        [] { throw std::runtime_error(std::string("cannot pause writer: ") + "\xCF\xB5\xCD\xB3\xD5\xD2"); }));
    job.wait_for_test();
    ASSERT_TRUE(job.progress());
    EXPECT_EQ(job.progress()->state, "failed");
    EXPECT_TRUE(acecode::is_valid_utf8(job.progress()->error)) << job.progress()->error;
    EXPECT_NE(job.progress()->error.find("cannot pause writer: "), std::string::npos);
}

namespace {

// 5 层、每层 60 字符的相对目录(约 305 字符),接在任何临时根后面都超过 MAX_PATH。
fs::path deep_relative_dir() {
    fs::path rel;
    for (int i = 0; i < 5; ++i) rel /= std::string(60, static_cast<char>('a' + i));
    return rel;
}

// 用扩展长度路径建目录 / 写文件:普通路径在 Windows 上建不出 >260 字符的文件。
void write_file_long(const fs::path& p, const std::string& content) {
    const fs::path io = acecode::to_extended_length_path(p);
    fs::create_directories(io.parent_path());
    std::ofstream ofs(io, std::ios::binary | std::ios::trunc);
    ofs << content;
}

std::string read_file_long(const fs::path& p) {
    std::ifstream ifs(acecode::to_extended_length_path(p), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

bool exists_long(const fs::path& p) {
    std::error_code ec;
    return fs::exists(acecode::to_extended_length_path(p), ec);
}

}  // namespace

// 场景:staging 目录名生成。
// 期望:以 kMigrationStagingPrefix 开头、总长 ≤ 21(前缀 13 + 8 位 hex),两次调用不同。
// 来由:原来是 ".acecode-migration-" + 完整 uuid,共 55 字符,staging 下的深层路径因此
// 比最终路径先顶破 MAX_PATH(260),正是 0923 反馈里「系统找不到指定的路径」的最后一截。
TEST_F(DataDirMigrationTest, StagingDirectoryNameIsShort) {
    const std::string a = make_migration_staging_name();
    const std::string b = make_migration_staging_name();
    EXPECT_EQ(a.rfind(kMigrationStagingPrefix, 0), 0u) << a;
    EXPECT_LE(a.size(), 21u) << a;
    EXPECT_NE(a, b);
}

// 场景:数据目录里有一个绝对路径超过 300 字符的文件(5 层 × 60 字符目录)。
// 期望:迁移成功(state=done),目标里同一相对路径的文件存在且内容一致。
// bug 表现:修复前 std::filesystem 走普通路径,受 MAX_PATH 限制,迁移报
// 「cannot copy …: 系统找不到指定的路径」(错误码 3),每次重试都必然失败。
TEST_F(DataDirMigrationTest, CopiesFilesBeyondMaxPath) {
    populate_source(default_dir);
    const fs::path rel = deep_relative_dir() / "deep.txt";
    const fs::path source_file = default_dir / rel;
    ASSERT_GT(s(source_file).size(), 300u) << "前提:源文件路径必须超过 MAX_PATH";
    write_file_long(source_file, "deep content");
    ASSERT_TRUE(exists_long(source_file));

    const fs::path target = root / "moved";
    const auto result = run_data_dir_migration(s(default_dir), s(default_dir), s(target));
    ASSERT_EQ(result.state, "done") << result.error;
    EXPECT_TRUE(exists_long(target / rel));
    EXPECT_EQ(read_file_long(target / rel), "deep content");
    EXPECT_TRUE(fs::exists(target / "config.json"));
}

// 场景:用户选择删除旧数据目录,旧目录里有超过 MAX_PATH 的文件(Agent 用 node / python
// 等长路径感知工具生成的深层产物)。
// 期望:cleanup_previous_data_dir 返回空串,旧目录整个被删除。
// bug 表现:修复前 remove_all 走普通路径删不掉深层文件,返回 CLEANUP_FAILED,旧目录残留。
TEST_F(DataDirMigrationTest, CleanupRemovesFilesBeyondMaxPath) {
    const fs::path old_dir = root / "old-elsewhere";
    write_file_long(old_dir / deep_relative_dir() / "deep.txt", "x");
    write_file(old_dir / "config.json", "{}");
    acecode::DataDirRedirect r;
    r.data_dir = s(root / "elsewhere");
    r.previous_data_dir = s(old_dir);
    r.cleanup_pending = true;
    ASSERT_TRUE(acecode::write_data_dir_redirect(s(default_dir), r));

    EXPECT_EQ(cleanup_previous_data_dir(s(old_dir), s(default_dir)), "");
    EXPECT_FALSE(exists_long(old_dir));
}

namespace {

// 造一个最小的 WebView2 profile(Desktop 的 Agent Browser user data):配置、会话库、
// 可再生缓存与锁文件。DIPS 用非 SQLite 内容,走原始复制路径。
// root_name 是数据根下 Agent Browser 目录的名字:旧装机 agent-browser,新装机 agent_browser。
fs::path populate_browser_profile(const fs::path& data_dir,
                                  const std::string& root_name = "agent-browser") {
    const fs::path profile = data_dir / root_name / "webview2";
    write_file(profile / "Local State", R"({"browser":{}})");
    write_file(profile / "lockfile", "");
    write_file(profile / "Default" / "Preferences", R"({"profile":{}})");
    write_file(profile / "Default" / "DIPS", "dips");
    write_file(profile / "Default" / "DIPS-shm", "shm");
    write_file(profile / "Default" / "Cache" / "Cache_Data" / "data_0", "cache");
    write_file(profile / "Default" / "Code Cache" / "js" / "index", "code cache");
    write_file(profile / "Default" / "Local Storage" / "leveldb" / "LOCK", "");
    write_file(profile / "Default" / "Local Storage" / "leveldb" / "000003.log", "leveldb");
    return profile;
}

void append_file(const fs::path& p, const std::string& content) {
    std::ofstream ofs(p, std::ios::binary | std::ios::app);
    ofs << content;
}

#ifdef _WIN32
// 以 share=0 独占打开文件,模拟被别的进程锁住;析构时关闭。
class ExclusiveFileLock {
public:
    explicit ExclusiveFileLock(const fs::path& p)
        : handle_(CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~ExclusiveFileLock() { if (valid()) CloseHandle(handle_); }
    ExclusiveFileLock(const ExclusiveFileLock&) = delete;
    ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE handle_;
};
#endif

}  // namespace

// 场景:Desktop 开着,Agent Browser 的 WebView2 profile 在迁移过程中持续变动 —— 追加写
// DIPS-shm、新建临时文件、删掉一个已清点的文件(第一次进度回调里做,此时刚清点完、还没复制)。
// 期望:迁移成功并写指针;目标里有 Local State,没有 Cache / Code Cache / lockfile / LOCK;
// 被删的文件属于 not-found,不计入 skipped_files(否则几乎每次迁移都会弹「需要重新登录」)。
// bug 表现:0912 反馈日志连续 8 次在复检阶段失败:
// 「source changed during migration: ...DIPS-shm」,只要 Desktop 开着迁移就必然失败。
TEST_F(DataDirMigrationTest, BrowserProfileChurnDoesNotFailMigration) {
    populate_source(default_dir);
    const fs::path profile = populate_browser_profile(default_dir);
    bool churned = false;
    const fs::path target = root / "moved";
    const auto result = run_data_dir_migration(s(default_dir), s(default_dir), s(target),
        [&](unsigned long long, unsigned long long) {
            if (churned) return;
            churned = true;
            append_file(profile / "Default" / "DIPS-shm", "more shared memory");
            write_file(profile / "Default" / "new.tmp", "tmp");
            fs::remove(profile / "Default" / "Preferences");
        });
    ASSERT_EQ(result.state, "done") << result.error;
    EXPECT_TRUE(churned);
    EXPECT_EQ(result.skipped_files, 0u);
    EXPECT_TRUE(acecode::read_data_dir_redirect(s(default_dir)).has_value());
    const fs::path moved = target / "agent-browser" / "webview2";
    EXPECT_TRUE(fs::exists(moved / "Local State"));
    EXPECT_TRUE(fs::exists(moved / "Default" / "DIPS"));
    EXPECT_TRUE(fs::exists(moved / "Default" / "Local Storage" / "leveldb" / "000003.log"));
    EXPECT_FALSE(fs::exists(moved / "Default" / "Preferences"));
    EXPECT_FALSE(fs::exists(moved / "Default" / "Cache"));
    EXPECT_FALSE(fs::exists(moved / "Default" / "Code Cache"));
    EXPECT_FALSE(fs::exists(moved / "lockfile"));
    EXPECT_FALSE(fs::exists(moved / "Default" / "Local Storage" / "leveldb" / "LOCK"));
    // 进度条要能走满:尽力子树的字节无论成败都计入 copied。
    EXPECT_EQ(result.copied_bytes, result.total_bytes);
}

// 场景:新装环境(2026-10-16 起运行数据改用下划线命名)的 Agent Browser 目录是
// agent_browser/webview2,Desktop 开着,profile 在迁移过程中持续变动 —— 与上一条用例相同的
// 追加写 / 新建临时文件 / 删除已清点文件。
// 期望:与连字符目录完全一致 —— 迁移成功并写指针,缓存与锁文件不复制,被删文件不计入
// skipped_files。
// bug 表现:修复前尽力复制只认 agent-browser/webview2,下划线目录被当成普通文件严格复检,
// 迁移报「source changed during migration: agent_browser/webview2/...」失败,新装用户只要
// Desktop 开着就迁移不了数据目录。
TEST_F(DataDirMigrationTest, UnderscoreBrowserProfileChurnDoesNotFailMigration) {
    populate_source(default_dir);
    const fs::path profile = populate_browser_profile(default_dir, "agent_browser");
    bool churned = false;
    const fs::path target = root / "moved";
    const auto result = run_data_dir_migration(s(default_dir), s(default_dir), s(target),
        [&](unsigned long long, unsigned long long) {
            if (churned) return;
            churned = true;
            append_file(profile / "Default" / "DIPS-shm", "more shared memory");
            write_file(profile / "Default" / "new.tmp", "tmp");
            fs::remove(profile / "Default" / "Preferences");
        });
    ASSERT_EQ(result.state, "done") << result.error;
    EXPECT_TRUE(churned);
    EXPECT_EQ(result.skipped_files, 0u);
    EXPECT_TRUE(acecode::read_data_dir_redirect(s(default_dir)).has_value());
    const fs::path moved = target / "agent_browser" / "webview2";
    EXPECT_TRUE(fs::exists(moved / "Local State"));
    EXPECT_TRUE(fs::exists(moved / "Default" / "DIPS"));
    EXPECT_TRUE(fs::exists(moved / "Default" / "Local Storage" / "leveldb" / "000003.log"));
    EXPECT_FALSE(fs::exists(moved / "Default" / "Cache"));
    EXPECT_FALSE(fs::exists(moved / "Default" / "Code Cache"));
    EXPECT_FALSE(fs::exists(moved / "lockfile"));
    EXPECT_EQ(result.copied_bytes, result.total_bytes);
}

// 场景:profile 里的 Cookies 库是 WAL 模式,浏览器连接一直开着,已提交的数据还在 -wal 里;
// 旁边还躺着一个 Cookies-journal。
// 期望:目标库能读到那一行(走了在线 backup 的一致快照);目标里没有 -wal 和 -journal。
// 这是 Agent Browser 登录态迁移后得以保留的前提。journal 首字节写 0:非零首字节会让
// SQLite 把它当热日志、只读打开直接拒绝(那种情况由原始复制兜底,不是本用例要测的)。
TEST_F(DataDirMigrationTest, BrowserProfileDatabaseIsSnapshotted) {
    populate_source(default_dir);
    const fs::path network = default_dir / "agent-browser" / "webview2" / "Default" / "Network";
    fs::create_directories(network);
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(s(network / "Cookies").c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "PRAGMA journal_mode=WAL; CREATE TABLE cookies(name TEXT); INSERT INTO cookies VALUES('session-cookie');", nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_TRUE(fs::exists(network / "Cookies-wal")) << "前提:已提交的数据留在 wal 里";
    write_file(network / "Cookies-journal", std::string("\0stale journal bytes", 20));

    const fs::path target = root / "moved";
    const auto result = run_data_dir_migration(s(default_dir), s(default_dir), s(target));
    sqlite3_close(db);
    ASSERT_EQ(result.state, "done") << result.error;
    EXPECT_EQ(result.skipped_files, 0u);

    const fs::path moved = target / "agent-browser" / "webview2" / "Default" / "Network";
    EXPECT_FALSE(fs::exists(moved / "Cookies-wal"));
    EXPECT_FALSE(fs::exists(moved / "Cookies-journal"));
    ASSERT_EQ(sqlite3_open(s(moved / "Cookies").c_str(), &db), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT name FROM cookies", -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)), "session-cookie");
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

// 场景:profile 里某个文件被别的进程独占锁住(share=0),原始复制读不了。
// 期望:迁移仍然成功,skipped_files == 1,目标里没有这个文件,其余 profile 内容照常复制。
// 注:这是防御性场景 —— 0912 实测并没有遇到锁,失败全在复检;但真遇到锁时不能让整个
// 迁移失败,最坏结果只是 Agent Browser 需要重新登录。
TEST_F(DataDirMigrationTest, LockedBrowserProfileFileIsSkipped) {
#ifdef _WIN32
    populate_source(default_dir);
    const fs::path profile = populate_browser_profile(default_dir);
    ExclusiveFileLock lock(profile / "Default" / "Preferences");
    ASSERT_TRUE(lock.valid());
    const fs::path target = root / "moved";
    const auto result = run_data_dir_migration(s(default_dir), s(default_dir), s(target));
    ASSERT_EQ(result.state, "done") << result.error;
    EXPECT_EQ(result.skipped_files, 1u);
    const fs::path moved = target / "agent-browser" / "webview2";
    EXPECT_FALSE(fs::exists(moved / "Default" / "Preferences"));
    EXPECT_TRUE(fs::exists(moved / "Local State"));
    EXPECT_TRUE(fs::exists(moved / "Default" / "DIPS"));
#else
    GTEST_SKIP() << "share-mode exclusive locks are Windows-only";
#endif
}

// 场景:profile 子树之外的文件(memory/MEMORY.md)被独占锁住。
// 期望:迁移失败且不写指针 —— 尽力复制只放宽 agent-browser/webview2 这一个子树,
// ACECode 自己的数据复制不全绝不能发布。这是防止尽力模式被扩大化的哨兵。
TEST_F(DataDirMigrationTest, LockedFileOutsideBrowserProfileStillFails) {
#ifdef _WIN32
    populate_source(default_dir);
    populate_browser_profile(default_dir);
    ExclusiveFileLock lock(default_dir / "memory" / "MEMORY.md");
    ASSERT_TRUE(lock.valid());
    const auto result = run_data_dir_migration(s(default_dir), s(default_dir), s(root / "moved"));
    EXPECT_EQ(result.state, "failed");
    EXPECT_NE(result.error.find("MEMORY.md"), std::string::npos) << result.error;
    EXPECT_FALSE(acecode::read_data_dir_redirect(s(default_dir)).has_value());
#else
    GTEST_SKIP() << "share-mode exclusive locks are Windows-only";
#endif
}

// 场景:webapp 兼容模式下 Edge --app 的 profile(edge-app-profile/u<pid>)在迁移中持续写入。
// 期望:迁移成功,目标里根本没有 edge-app-profile —— 它每次启动都重建,没有要保留的数据。
// bug 表现:修复前它和 WebView2 profile 一样会让变更复检必然失败。
TEST_F(DataDirMigrationTest, EdgeAppProfileChurnIsExcluded) {
    populate_source(default_dir);
    write_file(default_dir / "edge-app-profile" / "u1" / "Default" / "x", "edge");
    bool churned = false;
    const fs::path target = root / "moved";
    const auto result = run_data_dir_migration(s(default_dir), s(default_dir), s(target),
        [&](unsigned long long, unsigned long long) {
            if (churned) return;
            churned = true;
            append_file(default_dir / "edge-app-profile" / "u1" / "Default" / "x", "more");
            write_file(default_dir / "edge-app-profile" / "u1" / "Default" / "y", "new");
        });
    ASSERT_EQ(result.state, "done") << result.error;
    EXPECT_TRUE(churned);
    EXPECT_FALSE(fs::exists(target / "edge-app-profile"));
}

// 场景:无工作区会话目录 cache/no-workspace/<id>/ 下既有 Agent 的产出文件,也有
// .acecode/tmp 下的 ACECODE_TMPDIR 临时脚本 / 截图。
// 期望:产出文件 out.md 迁移过去;.acecode/tmp 不复制(bash 每次调用会按需重建)。
TEST_F(DataDirMigrationTest, NoWorkspaceScratchIsNotCopied) {
    populate_source(default_dir);
    const fs::path session = fs::path("cache") / "no-workspace" / "20260924-120000-abcd";
    write_file(default_dir / session / "out.md", "deliverable");
    write_file(default_dir / session / ".acecode" / "tmp" / "session-1" / "shot.png", "png");
    const fs::path target = root / "moved";
    const auto result = run_data_dir_migration(s(default_dir), s(default_dir), s(target));
    ASSERT_EQ(result.state, "done") << result.error;
    EXPECT_EQ(read_file_long(target / session / "out.md"), "deliverable");
    EXPECT_FALSE(fs::exists(target / session / ".acecode" / "tmp"));
}

// 场景:旧目录里有大量不迁移的数据 —— Agent Browser 的可再生缓存(Cache / Code Cache)、
// webapp 兼容模式的 edge-app-profile、无工作区会话的 .acecode/tmp,外加 run/、tmp/ 与锁文件。
// 期望:指针的 previous_size_bytes = 清点总量 + 这些留在旧目录里的字节之和,而不是复制量。
// bug 表现:修复前 previous_size_bytes 取 copied_bytes,排除项全被漏掉。例如 60 MB 会话数据
// + 300 MB 浏览器缓存,记成 60 MB,低于 100 MiB 的清理提示阈值,旧目录实际占着 360 MB
// 却永远不提示清理,清理界面报的大小也偏小。
TEST_F(DataDirMigrationTest, PreviousSizeCountsDataLeftBehind) {
    populate_source(default_dir);
    const fs::path profile = populate_browser_profile(default_dir);
    // 缓存写大一点(64 KiB),让「漏算」在断言里一眼可见。
    const std::string big_cache(64 * 1024, 'c');
    write_file(profile / "Default" / "Cache" / "Cache_Data" / "data_1", big_cache);
    write_file(default_dir / "edge-app-profile" / "u1" / "Default" / "x", "edge");
    const fs::path scratch = fs::path("cache") / "no-workspace" / "20260924-120000-abcd" /
                             ".acecode" / "tmp" / "session-1" / "shot.png";
    write_file(default_dir / scratch, "png");

    unsigned long long left_behind = 0;
    for (const fs::path rel : {fs::path("run/daemon.pid"), fs::path("tmp/scratch.txt"),
                               fs::path("config.json.lock"),
                               fs::path("agent-browser/webview2/lockfile"),
                               fs::path("agent-browser/webview2/Default/Cache/Cache_Data/data_0"),
                               fs::path("agent-browser/webview2/Default/Cache/Cache_Data/data_1"),
                               fs::path("agent-browser/webview2/Default/Code Cache/js/index"),
                               fs::path("agent-browser/webview2/Default/Local Storage/leveldb/LOCK"),
                               fs::path("edge-app-profile/u1/Default/x"), scratch}) {
        left_behind += fs::file_size(default_dir / rel);
    }
    ASSERT_GE(left_behind, big_cache.size());

    const fs::path target = root / "moved";
    const auto result = run_data_dir_migration(s(default_dir), s(default_dir), s(target));
    ASSERT_EQ(result.state, "done") << result.error;
    EXPECT_FALSE(fs::exists(target / "agent-browser" / "webview2" / "Default" / "Cache"));

    auto pointer = acecode::read_data_dir_redirect(s(default_dir));
    ASSERT_TRUE(pointer.has_value());
    EXPECT_EQ(pointer->previous_size_bytes, result.total_bytes + left_behind);
    EXPECT_GT(pointer->previous_size_bytes, result.copied_bytes + big_cache.size() - 1)
        << "缓存字节必须计入旧目录大小,否则清理提示阈值判断偏小";
}

// 场景:迁移过程中 ACECode 自己的数据目录里新出现一个文件(memory/new.md)。
// 期望:迁移失败(不发布不完整的数据),错误文本带上新增条目的相对路径。
// bug 表现:0912 反馈第一次失败只有「source changed during migration; retry when ACECode
// is idle」,没有路径,无从定位是谁在写。
TEST_F(DataDirMigrationTest, NewEntryDuringMigrationNamesThePath) {
    populate_source(default_dir);
    bool changed = false;
    const auto result = run_data_dir_migration(s(default_dir), s(default_dir), s(root / "moved"),
        [&](unsigned long long, unsigned long long) {
            if (changed) return;
            changed = true;
            write_file(default_dir / "memory" / "new.md", "new");
        });
    EXPECT_EQ(result.state, "failed");
    EXPECT_NE(result.error.find("memory"), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("new.md"), std::string::npos) << result.error;
    EXPECT_FALSE(acecode::read_data_dir_redirect(s(default_dir)).has_value());
}

// 场景:迁移 / 清理被 OTHER_INSTANCES_ACTIVE 拦下时,拒绝日志要说清是哪个 pid 文件拦的。
// 「造一个活着的别人的 pid」在集成层不好做,这里直接测 holder 的格式化函数。
// 期望:projects/<hash>/run 下的 pid 文件(旧版 per-workspace daemon 的遗留目录)标
// legacy=yes;顶层 run/desktop-shared 下的标 legacy=no;两者都带 pid 与文件路径。
// bug 表现:0913 反馈「移动工作目录提示程序占用」时日志里没有任何 [data-dir] 行,
// 分不清是真有别的实例,还是遗留 pid 被无关进程复用。
TEST_F(DataDirMigrationTest, HolderReportsLegacyRunDir) {
    const fs::path legacy_pid = default_dir / "projects" / "abc" / "run" / "daemon.pid";
    const std::string legacy = describe_daemon_pid_holder(default_dir, legacy_pid, 4242);
    EXPECT_NE(legacy.find("pid=4242"), std::string::npos) << legacy;
    EXPECT_NE(legacy.find("file=" + s(legacy_pid)), std::string::npos) << legacy;
    EXPECT_NE(legacy.find("legacy=yes"), std::string::npos) << legacy;

    const fs::path shared_pid = default_dir / "run" / "desktop-shared" / "daemon.pid";
    const std::string shared = describe_daemon_pid_holder(default_dir, shared_pid, 7);
    EXPECT_NE(shared.find("pid=7"), std::string::npos) << shared;
    EXPECT_NE(shared.find("legacy=no"), std::string::npos) << shared;

    // 没有任何活着的别的 daemon 时不拦截,holder 保持为空(populate_source 写的
    // run/daemon.pid 是 123;为免撞上真实进程,这里改成自己的 pid,必然被跳过)。
    write_file(default_dir / "run" / "daemon.pid", std::to_string(
#ifdef _WIN32
        static_cast<long long>(GetCurrentProcessId())
#else
        static_cast<long long>(getpid())
#endif
    ));
    std::string holder;
    EXPECT_FALSE(data_dir_has_other_daemons(s(default_dir), &holder));
    EXPECT_TRUE(holder.empty()) << holder;
}

namespace {

using acecode::daemon::DaemonProcessIdentity;
using acecode::daemon::RuntimeSnapshot;

constexpr std::int64_t kSelfPid = 100;
constexpr std::int64_t kHolderPid = 4242;
constexpr std::int64_t kHeartbeatMs = 1'800'000'000'000;

// 一份「pid 与心跳一致」的 runtime snapshot:心跳时间固定为 kHeartbeatMs。
RuntimeSnapshot snapshot_with_heartbeat(std::int64_t pid) {
    RuntimeSnapshot snapshot;
    snapshot.pid = pid;
    acecode::daemon::Heartbeat hb;
    hb.pid = pid;
    hb.timestamp_ms = kHeartbeatMs;
    snapshot.heartbeat = hb;
    return snapshot;
}

}  // namespace

// 场景:daemon.pid 记录的进程已经不在了(最常见的遗留文件)。
// 期望:不拦截。
TEST(EvaluateDaemonPidHolder, DeadPidDoesNotBlock) {
    const auto verdict = evaluate_daemon_pid_holder(snapshot_with_heartbeat(kHolderPid), kSelfPid,
        /*pid_alive=*/false, DaemonProcessIdentity::Unknown, std::nullopt);
    EXPECT_FALSE(verdict.blocks) << verdict.reason;
}

// 场景:daemon.pid 就是发起迁移的本进程(run/desktop-shared 下自己的 pid 文件)。
// 期望:不拦截,即使探测结果显示它是活着的 acecode。
TEST(EvaluateDaemonPidHolder, OwnPidDoesNotBlock) {
    const auto verdict = evaluate_daemon_pid_holder(snapshot_with_heartbeat(kSelfPid), kSelfPid,
        /*pid_alive=*/true, DaemonProcessIdentity::Match, kHeartbeatMs - 60'000);
    EXPECT_FALSE(verdict.blocks) << verdict.reason;
}

// 场景:pid 活着,但进程镜像已经不是 acecode(Mismatch)—— 遗留 pid 被无关进程复用。
// 期望:不拦截,reason 以 "pid reused" 开头。没有心跳也一样:Mismatch 本身就是证明。
// bug 表现:projects/*/run 下的遗留 pid 被 chrome 等进程复用后,迁移永远报「请关闭
// 其他 ACECode 窗口」,用户关掉所有窗口也没用。
TEST(EvaluateDaemonPidHolder, MismatchedImageDoesNotBlock) {
    RuntimeSnapshot no_heartbeat;
    no_heartbeat.pid = kHolderPid;
    const auto verdict = evaluate_daemon_pid_holder(no_heartbeat, kSelfPid,
        /*pid_alive=*/true, DaemonProcessIdentity::Mismatch, std::nullopt);
    EXPECT_FALSE(verdict.blocks) << verdict.reason;
    EXPECT_EQ(verdict.reason.rfind("pid reused", 0), 0u) << verdict.reason;
}

// 场景:pid 活着且镜像同名(甚至就是另一个 acecode),但进程启动时间晚于该目录的心跳。
// 期望:不拦截 —— 写心跳的那个 daemon 早已退出,这是后来复用了同一个 pid 的新进程。
// 阈值:runtime_pid_reuse_is_proven 默认 2 秒容差(平台时钟粒度),这里晚 5 秒,明确越过;
// 另取晚 1 秒的一例确认容差内仍按「未证明」拦截。
TEST(EvaluateDaemonPidHolder, ProcessStartedAfterHeartbeatDoesNotBlock) {
    const auto later = evaluate_daemon_pid_holder(snapshot_with_heartbeat(kHolderPid), kSelfPid,
        /*pid_alive=*/true, DaemonProcessIdentity::Match, kHeartbeatMs + 5'000);
    EXPECT_FALSE(later.blocks) << later.reason;
    EXPECT_EQ(later.reason.rfind("pid reused", 0), 0u) << later.reason;

    const auto within_tolerance = evaluate_daemon_pid_holder(snapshot_with_heartbeat(kHolderPid),
        kSelfPid, /*pid_alive=*/true, DaemonProcessIdentity::Match, kHeartbeatMs + 1'000);
    EXPECT_TRUE(within_tolerance.blocks) << within_tolerance.reason;
}

// 场景:pid 活着、镜像是 acecode、进程启动早于心跳 —— 真有另一个实例在用这个数据目录。
// 期望:拦截(OTHER_INSTANCES_ACTIVE 的正当情形)。
TEST(EvaluateDaemonPidHolder, LiveMatchingDaemonBlocks) {
    const auto verdict = evaluate_daemon_pid_holder(snapshot_with_heartbeat(kHolderPid), kSelfPid,
        /*pid_alive=*/true, DaemonProcessIdentity::Match, kHeartbeatMs - 60'000);
    EXPECT_TRUE(verdict.blocks) << verdict.reason;
}

// 场景:pid 活着,但身份读不出来(Unknown,如权限不足),目录里也没有心跳。
// 期望:拦截(fail-closed)—— 证明不了是复用,就不能冒着复制期间有人写的风险放行。
TEST(EvaluateDaemonPidHolder, UnknownIdentityWithoutHeartbeatBlocks) {
    RuntimeSnapshot no_heartbeat;
    no_heartbeat.pid = kHolderPid;
    const auto verdict = evaluate_daemon_pid_holder(no_heartbeat, kSelfPid,
        /*pid_alive=*/true, DaemonProcessIdentity::Unknown, kHeartbeatMs + 60'000);
    EXPECT_TRUE(verdict.blocks) << verdict.reason;
}
