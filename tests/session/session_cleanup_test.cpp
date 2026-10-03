// 覆盖 SessionManager::cleanup_old_sessions(TUI 退出时按 config.max_sessions
// 清理最旧的会话)。
//
// 子会话(spawn_subagent 后台任务、网状 agent)与普通会话一样长期保存,只跟随
// 主会话一起删除:名额只按主会话计,被清理的主会话连同它的子会话一起删除。
// 一旦回归:
//   - 子会话占名额 → 一次网状协作派出十几个 agent,就把用户的老会话全挤掉
//   - 只删主会话不删子会话 → 子会话变成永远清不掉的孤儿记录
//   - 父会话早已不在的孤儿子会话不计名额 → 它们永远不会被清理

#include <gtest/gtest.h>

#include "session/session_manager.hpp"
#include "session/session_storage.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

using acecode::SessionManager;
using acecode::SessionMeta;
using acecode::SessionStorage;

namespace {

fs::path make_temp_cwd(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
               ("acecode_session_cleanup_" + hint + "_" +
                std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// 手造一条落盘会话(meta + 空 jsonl);updated_at 决定新旧顺序。
void write_session(const std::string& project_dir,
                   const std::string& id,
                   const std::string& updated_at,
                   const std::string& parent = {}) {
    SessionMeta meta;
    meta.id = id;
    meta.cwd = "/tmp/x";
    meta.created_at = updated_at;
    meta.updated_at = updated_at;
    meta.message_count = 1;
    meta.parent_session_id = parent;
    SessionStorage::write_meta(SessionStorage::meta_path(project_dir, id), meta);
    std::ofstream(SessionStorage::session_path(project_dir, id)) << "";
}

bool session_exists(const std::string& project_dir, const std::string& id) {
    return fs::exists(SessionStorage::meta_path(project_dir, id)) ||
           fs::exists(SessionStorage::session_path(project_dir, id));
}

}  // namespace

// 场景: max_sessions=2,磁盘上有 3 个主会话(newest / middle / oldest),最新的
// 主会话派过两个子会话(更新时间比所有主会话都新),最旧的主会话有一个子会话。
// 期望: 只按主会话计名额 → 只清掉最旧的主会话,并连同它的子会话一起删除;
// 另外两个主会话和最新主会话的子会话全部保留。
// 回归: 旧实现把子会话也计入名额,两个新子会话会把 newest 与 middle 两个主会话都挤掉,
// 而 oldest 的子会话却可能留下来成为孤儿。
TEST(SessionCleanup, ChildrenDoNotUseQuotaAndDieWithTheirParent) {
    const auto cwd = make_temp_cwd("cascade");
    const auto project_dir = SessionStorage::get_project_dir(cwd.string());
    fs::remove_all(project_dir);
    fs::create_directories(project_dir);

    write_session(project_dir, "20261001-000005-newa", "2026-10-01T00:00:05Z");
    write_session(project_dir, "20261001-000004-mida", "2026-10-01T00:00:04Z");
    write_session(project_dir, "20261001-000001-olda", "2026-10-01T00:00:01Z");
    write_session(project_dir, "20261001-000006-kida", "2026-10-01T00:00:06Z",
                  "20261001-000005-newa");
    write_session(project_dir, "20261001-000007-kidb", "2026-10-01T00:00:07Z",
                  "20261001-000005-newa");
    write_session(project_dir, "20261001-000002-kidc", "2026-10-01T00:00:02Z",
                  "20261001-000001-olda");

    SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model");
    sm.cleanup_old_sessions(2);

    EXPECT_TRUE(session_exists(project_dir, "20261001-000005-newa"));
    EXPECT_TRUE(session_exists(project_dir, "20261001-000004-mida"))
        << "子会话不占名额,不能把较新的主会话挤掉";
    EXPECT_TRUE(session_exists(project_dir, "20261001-000006-kida"));
    EXPECT_TRUE(session_exists(project_dir, "20261001-000007-kidb"));
    EXPECT_FALSE(session_exists(project_dir, "20261001-000001-olda"));
    EXPECT_FALSE(session_exists(project_dir, "20261001-000002-kidc"))
        << "被清理主会话的子会话要一起删除";

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}

// 场景: max_sessions=1,磁盘上有 1 个主会话和 1 个更旧的孤儿子会话(它的父会话
// 早已被删掉,例如旧版本只删了主会话)。
// 期望: 孤儿按主会话计名额,超额后被清掉;唯一的主会话保留。
// 回归: 若孤儿一律当子会话处理,它的父会话永远不会再出现在清理名单里,
// 孤儿记录就再也清不掉。
TEST(SessionCleanup, OrphanChildrenCountAsTopLevel) {
    const auto cwd = make_temp_cwd("orphan");
    const auto project_dir = SessionStorage::get_project_dir(cwd.string());
    fs::remove_all(project_dir);
    fs::create_directories(project_dir);

    write_session(project_dir, "20261001-000009-tops", "2026-10-01T00:00:09Z");
    write_session(project_dir, "20261001-000003-orph", "2026-10-01T00:00:03Z",
                  "20261001-000000-gone");

    SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model");
    sm.cleanup_old_sessions(1);

    EXPECT_TRUE(session_exists(project_dir, "20261001-000009-tops"));
    EXPECT_FALSE(session_exists(project_dir, "20261001-000003-orph"))
        << "父会话已不在的孤儿子会话要按主会话计名额并被清理";

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}
