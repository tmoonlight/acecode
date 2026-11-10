#include <gtest/gtest.h>

#include "session/session_load_metrics.hpp"
#include "session/sidebar_tree.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace {
using namespace acecode;
using nlohmann::json;

json row(const std::string& id, const std::string& loop = {}, const std::string& created = {}) {
    return {{"id", id}, {"created_at", created.empty() ? id : created}, {"updated_at", id},
            {"loop_execution", {{"loop_id", loop}}}};
}

TEST(SidebarTree, BatchKeepsOrderForBeforeAfterAndInsideAcrossReload) {
    const auto records = json::array({row("1", "a"), row("2", "a"), row("3", "b"), row("normal")});
    for (const auto& placement : {"before", "after", "inside"}) {
        json layout;
        auto tree = build_sidebar_tree(records, layout);
        std::string error;
        ASSERT_TRUE(move_sidebar_nodes(tree, layout, {"session:2", "session:1"}, "loop:b", placement, error));
        tree = build_sidebar_tree(records, layout);
        const auto& siblings = std::string(placement) == "inside" ? tree.nodes.at("loop:b").children : tree.roots;
        auto first = std::find(siblings.begin(), siblings.end(), "session:2");
        ASSERT_NE(first, siblings.end());
        ASSERT_NE(first + 1, siblings.end());
        EXPECT_EQ(*(first + 1), "session:1");
        if (std::string(placement) == "after") EXPECT_EQ(*(first - 1), "loop:b");
    }
}

TEST(SidebarTree, InvalidBatchDoesNotPartiallyChangeTreeOrLayout) {
    const auto records = json::array({row("1", "a"), row("2", "a"), row("3", "b"), row("normal")});
    json layout;
    auto tree = build_sidebar_tree(records, layout);
    const auto roots = tree.roots;
    const auto children = tree.nodes.at("loop:a").children;
    std::string error;
    for (const auto& sources : std::vector<std::vector<std::string>>{
        {}, {"session:1", "missing"}, {"session:1", "session:1"},
        {"session:1", "loop:b"}, {"session:1", "loop:a"},
        {"session:normal", "loop:a"},
    }) {
        EXPECT_FALSE(move_sidebar_nodes(tree, layout, sources, "loop:b", "inside", error));
        EXPECT_TRUE(layout.is_null());
        EXPECT_EQ(tree.roots, roots);
        EXPECT_EQ(tree.nodes.at("loop:a").children, children);
        EXPECT_TRUE(tree.nodes.at("session:normal").parent.empty());
    }
}

TEST(SidebarTree, BatchIntoDetachedRunCreatesOnlyOneManualGroup) {
    const auto records = json::array({row("1"), row("2"), row("run", "a"), row("normal")});
    json layout;
    auto tree = build_sidebar_tree(records, layout);
    std::string error;
    ASSERT_TRUE(move_sidebar_node(tree, layout, "session:run", "session:normal", "before", error));
    ASSERT_TRUE(move_sidebar_nodes(tree, layout, {"session:2", "session:1"}, "session:run", "inside", error));
    tree = build_sidebar_tree(records, layout);
    EXPECT_EQ(tree.nodes.at("group:run").children,
              (std::vector<std::string>{"session:run", "session:2", "session:1"}));
}

TEST(SidebarTree, LatestCreatedRunRepresentsGroupEvenWhenOlderRunWasUpdated) {
    auto old = row("old", "daily", "2026-10-11");
    old["updated_at"] = "2026-10-14";
    auto tree = build_sidebar_tree(json::array({old, row("new", "daily", "2026-10-13"), row("normal")}), {});
    EXPECT_EQ(tree.roots.size(), 2u);
    EXPECT_EQ(tree.nodes.at("loop:daily").session_id, "new");
    EXPECT_TRUE(tree.nodes.at("loop:daily").group);
    EXPECT_EQ(tree.nodes.at("loop:daily").children, (std::vector<std::string>{"session:new", "session:old"}));
}

TEST(SidebarTree, DetachedRunStaysOutsideAfterReloadAndNewRun) {
    json records = json::array({row("1", "daily"), row("2", "daily"), row("normal")});
    json layout;
    auto tree = build_sidebar_tree(records, layout);
    std::string error;
    ASSERT_TRUE(move_sidebar_node(tree, layout, "session:1", "session:normal", "before", error));
    records.push_back(row("3", "daily"));
    tree = build_sidebar_tree(records, layout);
    EXPECT_TRUE(tree.nodes.at("session:1").parent.empty());
    EXPECT_EQ(tree.nodes.at("loop:daily").session_id, "3");
    EXPECT_EQ(tree.nodes.at("loop:daily").children, (std::vector<std::string>{"session:3", "session:2"}));
}

TEST(SidebarTree, LeafCanMoveBetweenGroupsAndSiblingOrderSurvivesReload) {
    auto records = json::array({row("1", "a"), row("2", "a"), row("3", "b"), row("4", "b")});
    json layout;
    auto tree = build_sidebar_tree(records, layout);
    std::string error;
    ASSERT_TRUE(move_sidebar_node(tree, layout, "session:1", "loop:b", "inside", error));
    ASSERT_TRUE(move_sidebar_node(tree, layout, "session:1", "session:3", "before", error));
    tree = build_sidebar_tree(records, layout);
    EXPECT_EQ(tree.nodes.at("session:1").parent, "loop:b");
    EXPECT_EQ(tree.nodes.at("loop:b").children, (std::vector<std::string>{"session:4", "session:1", "session:3"}));
}

TEST(SidebarTree, ParentMovesAsGroupButCannotBecomeAChild) {
    auto tree = build_sidebar_tree(json::array({row("1", "a"), row("2", "a"), row("3", "b"), row("4", "b")}), {});
    json layout;
    std::string error;
    ASSERT_TRUE(move_sidebar_node(tree, layout, "loop:a", "loop:b", "before", error));
    EXPECT_EQ(tree.roots.front(), "loop:a");
    EXPECT_EQ(tree.nodes.at("loop:a").children.size(), 2u);
    const auto saved = layout;
    EXPECT_FALSE(move_sidebar_node(tree, layout, "loop:a", "loop:b", "inside", error));
    EXPECT_FALSE(move_sidebar_node(tree, layout, "loop:a", "session:3", "before", error));
    EXPECT_FALSE(move_sidebar_node(tree, layout, "session:1", "session:3", "inside", error));
    EXPECT_EQ(layout, saved);
}

TEST(SidebarTree, OrdinaryRowsCannotAcceptChildrenButCanMoveIntoScheduledRoot) {
    auto records = json::array({row("1"), row("2"), row("3", "daily")});
    json layout;
    auto tree = build_sidebar_tree(records, layout);
    std::string error;
    ASSERT_TRUE(move_sidebar_node(tree, layout, "session:1", "session:2", "before", error));
    const auto saved = layout;
    const auto roots = tree.roots;
    EXPECT_FALSE(move_sidebar_node(tree, layout, "session:1", "session:2", "inside", error));
    EXPECT_EQ(error, "SIDEBAR_SCHEDULED_PARENT_ONLY");
    EXPECT_EQ(layout, saved);
    EXPECT_EQ(tree.roots, roots);
    EXPECT_TRUE(tree.nodes.at("session:2").children.empty());
    ASSERT_TRUE(move_sidebar_node(tree, layout, "session:1", "loop:daily", "inside", error));
    tree = build_sidebar_tree(records, layout);
    EXPECT_EQ(tree.nodes.at("session:1").parent, "loop:daily");
    EXPECT_TRUE(tree.nodes.at("loop:daily").scheduled);
}

TEST(SidebarTree, DetachedScheduledRootAcceptsChildrenAndMissingParentPromotesThem) {
    const auto records = json::array({row("1", "daily"), row("2", "daily"), row("normal")});
    json layout;
    auto tree = build_sidebar_tree(records, layout);
    std::string error;
    ASSERT_TRUE(move_sidebar_node(tree, layout, "session:1", "session:normal", "before", error));
    ASSERT_TRUE(move_sidebar_node(tree, layout, "session:normal", "session:1", "inside", error));
    tree = build_sidebar_tree(records, layout);
    EXPECT_EQ(tree.nodes.at("session:1").parent, "group:1");
    EXPECT_TRUE(tree.nodes.at("session:1").scheduled);
    EXPECT_EQ(tree.nodes.at("session:normal").parent, "group:1");
    EXPECT_TRUE(tree.nodes.at("group:1").group);
    EXPECT_EQ(tree.nodes.at("group:1").loop_id, "daily");
    tree = build_sidebar_tree(json::array({row("2", "daily"), row("normal")}), layout);
    EXPECT_TRUE(tree.nodes.at("session:normal").parent.empty());
}

TEST(SidebarTree, LegacyOrdinaryParentsPromoteTheirChildren) {
    const json layout = {{"parents", {{"session:1", "session:normal"}, {"session:2", "session:normal"}}}};
    const auto tree = build_sidebar_tree(json::array({row("1", "daily"), row("2"), row("normal")}), layout);
    EXPECT_EQ(tree.roots.size(), 3u);
    for (const auto& [key, node] : tree.nodes) {
        EXPECT_TRUE(node.parent.empty());
        EXPECT_TRUE(node.children.empty());
    }
    EXPECT_TRUE(tree.nodes.at("session:1").scheduled);
}

TEST(SidebarTree, RemovingRepresentativeAndInvalidLayoutsKeepAllSurvivors) {
    auto tree = build_sidebar_tree(json::array({row("1", "a"), row("2", "a")}), {});
    EXPECT_EQ(tree.nodes.at("loop:a").session_id, "2");
    tree = build_sidebar_tree(json::array({row("1", "a")}), {});
    EXPECT_EQ(tree.nodes.at("loop:a").session_id, "1");
    json layout = {{"parents", {{"session:1", "session:2"}, {"session:2", "session:1"}, {"session:3", "session:2"}}}};
    tree = build_sidebar_tree(json::array({row("1", "a"), row("2", "b"), row("3", "c")}), layout);
    EXPECT_EQ(std::count_if(tree.nodes.begin(), tree.nodes.end(), [](const auto& entry) { return !entry.second.group; }), 3);
    for (const auto& [key, node] : tree.nodes) {
        if (!node.parent.empty()) EXPECT_TRUE(tree.nodes.at(node.parent).parent.empty());
    }
}

TEST(SidebarTree, SingleExecutionCanBeAttachedWithoutCapturingFutureRuns) {
    auto records = json::array({row("1", "daily"), row("receiver", "other")});
    json layout;
    auto tree = build_sidebar_tree(records, layout);
    std::string error;
    EXPECT_FALSE(move_sidebar_node(tree, layout, "loop:daily", "loop:other", "inside", error));
    ASSERT_TRUE(move_sidebar_node(tree, layout, "session:1", "loop:other", "inside", error));
    EXPECT_EQ(tree.nodes.at("session:1").parent, "loop:other");
    records.push_back(row("2", "daily"));
    tree = build_sidebar_tree(records, layout);
    EXPECT_EQ(tree.nodes.at("loop:daily").session_id, "2");
    EXPECT_EQ(tree.nodes.at("session:1").parent, "loop:other");
    EXPECT_EQ(tree.nodes.at("loop:daily").children, std::vector<std::string>{"session:2"});
}

TEST(SidebarTree, LegacyScheduledParentBecomesAStableManualGroup) {
    const auto records = json::array({row("anchor", "daily"), row("child"), row("normal")});
    json layout = {{"parents", {{"session:anchor", ""}, {"session:child", "session:anchor"}, {"session:archived", "session:anchor"}}},
                   {"orders", {{"", json::array({"session:anchor", "session:normal"})}}}};
    auto tree = build_sidebar_tree(records, layout);
    EXPECT_EQ(tree.roots, (std::vector<std::string>{"group:anchor", "session:normal"}));
    EXPECT_EQ(tree.nodes.at("group:anchor").children.size(), 2u);
    EXPECT_EQ(tree.nodes.at("session:anchor").parent, "group:anchor");
    EXPECT_EQ(tree.nodes.at("session:child").parent, "group:anchor");
    std::string error;
    ASSERT_TRUE(move_sidebar_node(tree, layout, "session:anchor", "session:normal", "before", error));
    tree = build_sidebar_tree(records, layout);
    EXPECT_TRUE(tree.nodes.at("session:anchor").parent.empty());
    EXPECT_EQ(tree.nodes.at("group:anchor").children, std::vector<std::string>{"session:child"});
    EXPECT_EQ(layout["version"], 2);
    EXPECT_EQ(layout["parents"]["session:archived"], "group:anchor");
    auto restored = records;
    restored.push_back(row("archived"));
    tree = build_sidebar_tree(restored, layout);
    EXPECT_TRUE(tree.nodes.at("session:anchor").parent.empty());
    EXPECT_EQ(tree.nodes.at("session:archived").parent, "group:anchor");
}

TEST(SidebarTree, LatestExecutionCanDetachAndGroupKeepsItsIdentity) {
    const auto records = json::array({row("1", "daily"), row("2", "daily"), row("normal")});
    json layout;
    auto tree = build_sidebar_tree(records, layout);
    std::string error;
    ASSERT_TRUE(move_sidebar_node(tree, layout, "session:2", "session:normal", "before", error));
    tree = build_sidebar_tree(records, layout);
    EXPECT_EQ(tree.nodes.at("loop:daily").session_id, "1");
    EXPECT_EQ(tree.nodes.at("loop:daily").children, std::vector<std::string>{"session:1"});
    EXPECT_TRUE(tree.nodes.at("session:2").parent.empty());
}

TEST(SidebarTree, LayoutPersistenceAndMetadataCacheObserveUpdatesAndDeletion) {
    const auto dir = std::filesystem::temp_directory_path() / ("ace-sidebar-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{dir};
    std::string error;
    const json layout = {{"parents", {{"session:1", ""}}}, {"orders", {{"", json::array({"session:1"})}}}};
    ASSERT_TRUE(write_sidebar_layout(dir / "sidebar_sessions.json", layout, error));
    EXPECT_EQ(read_sidebar_layout(dir / "sidebar_sessions.json"), layout);
    SessionMeta meta;
    meta.id = "one";
    meta.title = "before";
    const auto path = path_to_utf8(dir / "one.meta.json");
    ASSERT_TRUE(SessionStorage::write_meta(path, meta));
    SidebarMetadataCache cache;
    auto rows = cache.read({path_to_utf8(dir)});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].title, "before");
    meta.title = "after with a different size";
    ASSERT_TRUE(SessionStorage::write_meta(path, meta));
    rows = cache.read({path_to_utf8(dir)});
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].title, meta.title);
    std::filesystem::remove(dir / "one.meta.json");
    EXPECT_TRUE(cache.read({path_to_utf8(dir)}).empty());
}

// 「最近会话」查询的测试夹具:临时根目录下每个子目录当一个项目目录用。
class RecentSessions : public testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
                ("ace-recent-" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(root_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }
    std::string dir(const std::string& name) {
        std::filesystem::create_directories(root_ / name);
        return path_to_utf8(root_ / name);
    }
    // 在目录里落一份 meta;updated_at 为空时只写 created_at。
    void put(const std::string& directory, const std::string& id, const std::string& updated_at,
             bool no_workspace = false, const std::string& title = {}) {
        SessionMeta meta;
        meta.id = id;
        meta.title = title.empty() ? id : title;
        meta.created_at = "2020-11-05T00:00:00Z";
        meta.updated_at = updated_at;
        meta.no_workspace = no_workspace;
        ASSERT_TRUE(SessionStorage::write_meta(SessionStorage::meta_path(directory, id), meta));
    }
    static std::vector<std::string> ids(const std::vector<RecentSession>& sessions) {
        std::vector<std::string> out;
        for (const auto& session : sessions) out.push_back(session.meta.id);
        return out;
    }
    std::filesystem::path root_;
};

// 场景:反馈弹窗要「最近的几条会话」,来源是两个工作区的项目目录加两个无工作区
// 会话目录,里面混着已归档的、子代理的子会话、没有 updated_at 的,以及无工作区目录
// 里一条不属于它的记录。
// 期望:只收顶层、未归档的会话;按 updated_at 倒序(缺省用 created_at);无工作区
// 会话的工作区哈希为空串,其余带所在工作区的哈希;截到 limit。
TEST_F(RecentSessions, PicksNewestTopLevelUnarchivedSessionsAcrossDirectories) {
    const auto a = dir("ws-a"), b = dir("ws-b"), task1 = dir("task-1"), task2 = dir("task-2");
    put(a, "s1", "2020-11-01T00:00:00Z");
    put(b, "s2", "2020-11-03T00:00:00Z");
    put(b, "s-created-only", "");                  // 排序取 created_at = 2020-11-05
    put(task1, "t1", "2020-11-04T00:00:00Z", true);
    put(task2, "t2", "2020-11-02T00:00:00Z", true);
    put(task1, "stray", "2020-11-10T00:00:00Z");   // 无工作区目录里的普通记录,不算
    {
        SessionMeta archived;
        archived.id = "archived";
        archived.updated_at = "2020-11-09T00:00:00Z";
        archived.archived = true;
        ASSERT_TRUE(SessionStorage::write_meta(SessionStorage::meta_path(a, archived.id), archived));
        SessionMeta child;
        child.id = "child";
        child.updated_at = "2020-11-08T00:00:00Z";
        child.parent_session_id = "s1";
        ASSERT_TRUE(SessionStorage::write_meta(SessionStorage::meta_path(a, child.id), child));
    }
    const std::vector<SessionDirectoryScope> scopes = {
        {a, "aaaa", false}, {b, "bbbb", false}, {task1, "", true}, {task2, "", true}};
    SidebarMetadataCache cache;

    const auto all = cache.recent(scopes, 10);
    EXPECT_EQ(ids(all), (std::vector<std::string>{"s-created-only", "t1", "s2", "t2", "s1"}));
    ASSERT_EQ(all.size(), 5u);
    EXPECT_EQ(all[0].workspace_hash, "bbbb");
    EXPECT_EQ(all[1].workspace_hash, "");
    EXPECT_TRUE(all[1].meta.no_workspace);
    EXPECT_EQ(all[4].workspace_hash, "aaaa");

    EXPECT_EQ(ids(cache.recent(scopes, 3)),
              (std::vector<std::string>{"s-created-only", "t1", "s2"}));
    EXPECT_TRUE(cache.recent(scopes, 0).empty());
}

// 场景:活跃会话已经由调用方单独列出,磁盘上的同一条要跳过;另外同一个目录被
// 列了两次(两个工作区解析到同一个项目目录)。
// 期望:accept 拒掉的记录不占 limit 名额(后面的会话补上来);重复目录只看第一次,
// 结果里没有重复会话。
TEST_F(RecentSessions, RejectedRecordsFreeTheirSlotAndRepeatedDirectoriesAreReadOnce) {
    const auto a = dir("ws-a");
    put(a, "s1", "2020-11-01T00:00:00Z");
    put(a, "s2", "2020-11-02T00:00:00Z");
    put(a, "s3", "2020-11-03T00:00:00Z");
    const std::vector<SessionDirectoryScope> scopes = {{a, "aaaa", false}, {a, "aaaa", false}};
    SidebarMetadataCache cache;

    const auto picked = cache.recent(scopes, 2, [](const SessionMeta& meta, const std::string& hash) {
        EXPECT_EQ(hash, "aaaa");
        return meta.id != "s3";
    });
    EXPECT_EQ(ids(picked), (std::vector<std::string>{"s2", "s1"}));
    EXPECT_EQ(ids(cache.recent(scopes, 10)), (std::vector<std::string>{"s3", "s2", "s1"}));
}

// 场景:3 个工作区各 2 个会话,加 5 个无工作区会话(每个会话一个目录),连续两次
// 打开反馈弹窗;之后其中一个会话被改名。
// 期望:第一次每个 meta 文件只打开一次(11 个);第二次一个文件都不打开;改名后只
// 重读那一个,结果是新标题;侧栏随后用同一份缓存也不再读盘。
// 回归:旧实现每次打开弹窗都把所有工作区的全部会话文件重读一遍,无工作区会话还
// 按工作区个数重复读 —— 20 个工作区 + 372 个无工作区会话时,为了下拉框里的 20 条
// 要打开 12648 个文件、等 7.2 秒。
TEST_F(RecentSessions, ReadsEachMetaOnceThenServesFromTheSharedCache) {
    std::vector<SessionDirectoryScope> scopes;
    std::vector<std::string> directories;
    for (int index = 0; index < 3; ++index) {
        const auto workspace = dir("ws-" + std::to_string(index));
        put(workspace, "w" + std::to_string(index) + "a", "2020-11-01T00:00:0" + std::to_string(index) + "Z");
        put(workspace, "w" + std::to_string(index) + "b", "2020-11-02T00:00:0" + std::to_string(index) + "Z");
        scopes.push_back({workspace, "hash" + std::to_string(index), false});
        directories.push_back(workspace);
    }
    for (int index = 0; index < 5; ++index) {
        const auto task = dir("task-" + std::to_string(index));
        put(task, "t" + std::to_string(index), "2020-11-03T00:00:0" + std::to_string(index) + "Z", true);
        scopes.push_back({task, "", true});
        directories.push_back(task);
    }
    SidebarMetadataCache cache;
    const auto opened_by = [&](const std::function<void()>& action) {
        const auto before = session_read_metrics().files;
        action();
        return session_read_metrics().files - before;
    };

    std::vector<RecentSession> first, second, third;
    EXPECT_EQ(opened_by([&] { first = cache.recent(scopes, 4); }), 11u);
    EXPECT_EQ(ids(first), (std::vector<std::string>{"t4", "t3", "t2", "t1"}));
    EXPECT_EQ(opened_by([&] { second = cache.recent(scopes, 4); }), 0u);
    EXPECT_EQ(ids(second), ids(first));

    put(scopes[7].project_dir, "t4", "2020-11-04T00:00:00Z", true, "renamed with a longer title");
    EXPECT_EQ(opened_by([&] { third = cache.recent(scopes, 4); }), 1u);
    ASSERT_FALSE(third.empty());
    EXPECT_EQ(third.front().meta.id, "t4");
    EXPECT_EQ(third.front().meta.title, "renamed with a longer title");

    std::vector<SessionMeta> sidebar;
    EXPECT_EQ(opened_by([&] { sidebar = cache.read(directories); }), 0u);
    EXPECT_EQ(sidebar.size(), 11u);
}

// 场景:daemon 刚启动、缓存是空的,一个工作区里有 30 个很久没动过的旧会话(meta 的
// 修改时间是十年前)和 3 个最近几小时更新的会话;先打开反馈弹窗取最近 3 条,然后
// 侧栏加载这个工作区,再取最近 5 条。
// 期望:第一次只打开那 3 个新文件 —— meta 的修改时间不会早于里面记的 updated_at,
// 修改时间比当前第 3 名还旧的文件不可能排进来,不用读;没读的 30 个留给侧栏读;
// 之后取 5 条不再读盘,结果是 3 条新的加上旧会话里最新的两条。
// 回归:改成走缓存的第一版在冷启动时把所有 meta 全部解析一遍,真实数据上第一次
// 打开弹窗要读 5423 个文件。
TEST_F(RecentSessions, ColdListingOpensOnlyFilesRecentEnoughToRank) {
    using namespace std::chrono;
    const auto workspace = dir("ws");
    const auto ten_years_ago = std::filesystem::file_time_type::clock::now() - hours(24 * 365 * 10);
    for (int index = 0; index < 30; ++index) {
        const std::string id = "old" + std::string(index < 10 ? "0" : "") + std::to_string(index);
        put(workspace, id, "2000-01-01T00:00:" + std::string(index < 10 ? "0" : "") +
                               std::to_string(index) + "Z");
        std::filesystem::last_write_time(
            path_from_utf8(SessionStorage::meta_path(workspace, id)), ten_years_ago);
    }
    // 新会话的 updated_at 取真实的「几小时前」,这样不依赖测试机的日期。
    const auto hours_ago = [](int count) {
        const std::time_t at = system_clock::to_time_t(system_clock::now() - hours(count));
        std::tm tm_buf{};
#ifdef _WIN32
        gmtime_s(&tm_buf, &at);
#else
        gmtime_r(&at, &tm_buf);
#endif
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                      tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                      tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
        return std::string(buffer);
    };
    put(workspace, "new1", hours_ago(3));
    put(workspace, "new2", hours_ago(2));
    put(workspace, "new3", hours_ago(1));
    const std::vector<SessionDirectoryScope> scopes = {{workspace, "aaaa", false}};
    SidebarMetadataCache cache;
    const auto opened_by = [](const std::function<void()>& action) {
        const auto before = session_read_metrics().files;
        action();
        return session_read_metrics().files - before;
    };

    std::vector<RecentSession> top3, top5;
    EXPECT_EQ(opened_by([&] { top3 = cache.recent(scopes, 3); }), 3u);
    EXPECT_EQ(ids(top3), (std::vector<std::string>{"new3", "new2", "new1"}));

    std::vector<SessionMeta> sidebar;
    EXPECT_EQ(opened_by([&] { sidebar = cache.read({workspace}); }), 30u);
    EXPECT_EQ(sidebar.size(), 33u);

    EXPECT_EQ(opened_by([&] { top5 = cache.recent(scopes, 5); }), 0u);
    EXPECT_EQ(ids(top5), (std::vector<std::string>{"new3", "new2", "new1", "old29", "old28"}));
}
} // namespace
