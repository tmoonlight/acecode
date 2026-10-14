#include <gtest/gtest.h>

#include "session/sidebar_tree.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <filesystem>
#include <random>

namespace {
using namespace acecode;
using nlohmann::json;

json row(const std::string& id, const std::string& loop = {}, const std::string& created = {}) {
    return {{"id", id}, {"created_at", created.empty() ? id : created}, {"updated_at", id},
            {"loop_execution", {{"loop_id", loop}}}};
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
} // namespace
