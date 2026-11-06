// 本文件覆盖定时任务「旧通知清理」(loop_session_cleanup):每次新一轮运行开始后,
// 把这个任务更早的执行会话归档或永久删除到只剩最近 keep_recent_sessions 个。
//
// 场景:
//   1. select_old_loop_sessions 纯选择:不可见的不计数、在跑的占名额但不处理。
//   2. 开关映射:删除优先于归档(两者互斥,存储层已拒绝同时打开)。
//   3. 归档模式:超出保留数的最旧会话写 archived=true,置顶 / 已归档的不计数也不动。
//   4. 删除模式:超出保留数的会话连同子会话的文件一起删掉,保留的原样留着。
//   5. 两个开关都关:什么都不碰。
//   6. 存储位置:工作区任务落在工作区项目目录,无工作区任务每个会话一个缓存目录。
//
// 会话都写在临时目录里(通过 LoopSessionStorage 指过去),不碰真实 ~/.acecode。

#include "loop/loop_session_cleanup.hpp"

#include "permissions/permissions.hpp"
#include "session/session_client.hpp"
#include "session/session_pin_store.hpp"
#include "session/session_storage.hpp"
#include "session_host/session_registry.hpp"
#include "tool/tool_executor.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>

namespace acecode::loop {
namespace {

namespace fs = std::filesystem;

class RecordingSessionClient final : public SessionClient {
public:
    std::string create_session(const SessionOptions&) override { return {}; }
    bool resume_session(const std::string&, const SessionOptions&) override { return false; }
    std::vector<SessionInfo> list_sessions() override { return {}; }
    void destroy_session(const std::string& id) override { destroyed.push_back(id); }
    SubscriptionId subscribe(const std::string&, EventListener, std::uint64_t) override { return 0; }
    void unsubscribe(const std::string&, SubscriptionId) override {}
    bool send_input(const std::string&, const std::string&) override { return false; }
    BuiltinCommandResult execute_builtin_command(
        const std::string&, const BuiltinCommandRequest&) override { return {}; }
    void respond_permission(const std::string&, const PermissionDecision&) override {}
    QuestionResponseStatus respond_question(
        const std::string&, const std::string&,
        const AskUserQuestionResponse&) override {
        return QuestionResponseStatus::Closed;
    }
    void abort(const std::string&) override {}

    std::vector<std::string> destroyed;
};

class LoopSessionCleanupTest : public testing::Test {
protected:
    fs::path root = fs::temp_directory_path() / ("acecode-loop-cleanup-" + generate_uuid());
    std::string project_dir;
    ToolExecutor tools;
    PermissionManager permissions;
    std::unique_ptr<SessionRegistry> registry;
    RecordingSessionClient client;
    LoopSessionStorage storage;

    void SetUp() override {
        fs::create_directories(root / "project");
        project_dir = path_to_utf8(root / "project");
        SessionRegistryDeps deps;
        deps.tools = &tools;
        deps.cwd = path_to_utf8(root);
        deps.no_workspace_cache_root = path_to_utf8(root / "no-workspace");
        deps.template_permissions = &permissions;
        registry = std::make_unique<SessionRegistry>(std::move(deps));
        storage.project_dir = [dir = project_dir](const std::string&) { return dir; };
        storage.pinned_sessions_path = root / "project" / "pinned_sessions.json";
    }

    void TearDown() override {
        registry.reset();
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void write_session(const std::string& id, bool archived = false,
                       const std::string& parent = {}) {
        SessionMeta meta;
        meta.id = id;
        meta.cwd = project_dir;
        meta.created_at = "2026-11-05T00:00:00Z";
        meta.updated_at = meta.created_at;
        meta.archived = archived;
        meta.parent_session_id = parent;
        meta.loop_id = "loop-1";
        ASSERT_TRUE(SessionStorage::write_meta(SessionStorage::meta_path(project_dir, id), meta));
        std::ofstream(path_from_utf8(SessionStorage::session_path(project_dir, id)))
            << "{\"role\":\"user\",\"content\":\"hi\"}\n";
    }

    SessionMeta meta_of(const std::string& id) const {
        return SessionStorage::read_meta(SessionStorage::meta_path(project_dir, id));
    }

    bool exists(const std::string& id) const {
        return !meta_of(id).id.empty();
    }

    static LoopDefinition loop_with(bool archive, bool remove, int keep) {
        LoopDefinition loop;
        loop.id = "loop-1";
        loop.auto_archive_old_sessions = archive;
        loop.auto_delete_old_sessions = remove;
        loop.keep_recent_sessions = keep;
        return loop;
    }
};

// 场景 1:从新到旧 s6(在跑)、s5(已归档)、s4、s3(置顶)、s2(在跑)、s1,保留 2 个。
// 期望:s5 / s3 不可见,不占名额;名额给 s6 和 s4;s2 超出但在跑,本轮跳过;只返回 s1。
TEST(LoopSessionCleanupSelect, InvisibleSessionsAreSkippedAndBusyOnesKept) {
    const std::vector<LoopSessionState> sessions = {
        {"s6", true, true}, {"s5", false, false}, {"s4", true, false},
        {"s3", false, false}, {"s2", true, true}, {"s1", true, false},
    };
    EXPECT_EQ(select_old_loop_sessions(sessions, 2), (std::vector<std::string>{"s1"}));
    EXPECT_EQ(select_old_loop_sessions(sessions, 4), (std::vector<std::string>{}));
    EXPECT_EQ(select_old_loop_sessions(sessions, 1), (std::vector<std::string>{"s4", "s1"}));
}

// 场景 2:开关组合到清理方式的映射。两者同时为 true 在存储层已被拒绝,这里兜底
// 也按删除处理,保证结果确定。
TEST(LoopSessionCleanupSelect, ModeFollowsSwitches) {
    LoopDefinition loop;
    EXPECT_EQ(session_cleanup_mode(loop), SessionCleanupMode::Off);
    loop.auto_archive_old_sessions = true;
    EXPECT_EQ(session_cleanup_mode(loop), SessionCleanupMode::Archive);
    loop.auto_delete_old_sessions = true;
    EXPECT_EQ(session_cleanup_mode(loop), SessionCleanupMode::Delete);
}

// 场景 3:归档模式保留 2 个。运行记录从新到旧:new(刚建,还没落盘但在内存里——
// 这里用磁盘 meta 模拟已存在)、pinned(用户置顶)、mid、gone(文件已被用户删掉)、
// old、older(早已归档)。
// 期望:new / mid 保留;pinned 不计数不动;gone 不存在跳过;old 被归档;
// older 已归档、不重复写。修复前(没有这个功能)侧栏里旧通知会一直堆积。
TEST_F(LoopSessionCleanupTest, ArchiveModeArchivesSessionsBeyondKeepCount) {
    write_session("new");
    write_session("pinned");
    write_session("mid");
    write_session("old");
    write_session("older", true);
    session_pins::PinnedSessionsState pins;
    pins.session_ids = {"pinned"};
    ASSERT_TRUE(session_pins::write_pinned_sessions_state(storage.pinned_sessions_path, pins));

    const auto result = cleanup_old_loop_sessions(
        loop_with(true, false, 2), {"new", "pinned", "mid", "gone", "old", "older"},
        storage, *registry, client);

    EXPECT_EQ(result.archived, (std::vector<std::string>{"old"}));
    EXPECT_TRUE(result.deleted.empty());
    EXPECT_FALSE(meta_of("new").archived);
    EXPECT_FALSE(meta_of("pinned").archived);
    EXPECT_FALSE(meta_of("mid").archived);
    EXPECT_TRUE(meta_of("old").archived);
    EXPECT_TRUE(meta_of("older").archived);
    // 这些会话都不在内存里,不应该去销毁任何东西。
    EXPECT_TRUE(client.destroyed.empty());
}

// 场景 4:删除模式保留 1 个。old 有一个后台子会话 child(parent_session_id=old)。
// 期望:latest 留着;mid、old 连同 child 的 meta 与 transcript 都被删掉;
// 子会话本身不在运行记录里,只随主会话一起走。
TEST_F(LoopSessionCleanupTest, DeleteModeRemovesOldSessionsWithTheirChildren) {
    write_session("latest");
    write_session("mid");
    write_session("old");
    write_session("child", false, "old");

    const auto result = cleanup_old_loop_sessions(
        loop_with(false, true, 1), {"latest", "mid", "old"}, storage, *registry, client);

    EXPECT_EQ(result.deleted, (std::vector<std::string>{"mid", "old"}));
    EXPECT_TRUE(result.archived.empty());
    EXPECT_TRUE(exists("latest"));
    EXPECT_FALSE(exists("mid"));
    EXPECT_FALSE(exists("old"));
    EXPECT_FALSE(exists("child"));
    EXPECT_FALSE(fs::exists(path_from_utf8(SessionStorage::session_path(project_dir, "old"))));
    EXPECT_FALSE(fs::exists(path_from_utf8(SessionStorage::session_path(project_dir, "child"))));
}

// 场景 5:两个开关都关(默认),即使会话远超 3 个也一个都不动。
TEST_F(LoopSessionCleanupTest, OffModeLeavesEverySessionAlone) {
    std::vector<std::string> ids;
    for (int i = 0; i < 6; ++i) {
        ids.push_back("s" + std::to_string(i));
        write_session(ids.back());
    }
    const auto result = cleanup_old_loop_sessions(
        loop_with(false, false, 3), ids, storage, *registry, client);
    EXPECT_TRUE(result.archived.empty());
    EXPECT_TRUE(result.deleted.empty());
    for (const auto& id : ids) {
        EXPECT_TRUE(exists(id)) << id;
        EXPECT_FALSE(meta_of(id).archived) << id;
    }
}

// 场景 6:存储位置。工作区任务所有会话都在工作区项目目录(与 Web 侧栏同源),
// 置顶清单是同目录的 pinned_sessions.json;无工作区任务每个会话一个缓存目录,
// 置顶清单在缓存根。只比较路径,不落盘。
TEST(LoopSessionCleanupStorage, WorkspaceAndNoWorkspaceLocations) {
    LoopDefinition with_workspace;
    with_workspace.workspace_cwd = "C:/repo";
    const auto workspace = loop_session_storage(with_workspace, "C:/cache/no-workspace");
    const auto workspace_dir = SessionStorage::get_project_dir("C:/repo");
    EXPECT_EQ(workspace.project_dir("a"), workspace_dir);
    EXPECT_EQ(workspace.project_dir("b"), workspace_dir);
    EXPECT_EQ(workspace.pinned_sessions_path, path_from_utf8(workspace_dir) / "pinned_sessions.json");

    const LoopDefinition no_workspace;
    const auto cache = loop_session_storage(no_workspace, "C:/cache/no-workspace");
    EXPECT_EQ(cache.project_dir("a"),
              SessionStorage::get_project_dir(no_workspace_session_cwd("a", "C:/cache/no-workspace")));
    EXPECT_NE(cache.project_dir("a"), cache.project_dir("b"));
    EXPECT_EQ(cache.pinned_sessions_path,
              path_from_utf8("C:/cache/no-workspace") / "pinned_sessions.json");
}

} // namespace
} // namespace acecode::loop
