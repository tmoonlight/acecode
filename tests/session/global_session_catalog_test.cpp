#include <gtest/gtest.h>

#include "session/global_session_catalog.hpp"
#include "session/session_storage.hpp"
#include "utils/cwd_hash.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

class TempProjectsRoot {
public:
    TempProjectsRoot() {
        path_ = fs::temp_directory_path() /
            ("acecode_global_session_catalog_" +
             std::to_string(std::random_device{}()));
        fs::remove_all(path_);
        fs::create_directories(path_);
    }

    ~TempProjectsRoot() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

struct ProjectFixture {
    std::string cwd;
    std::string hash;
    fs::path path;
};

ProjectFixture project(TempProjectsRoot& root,
                       std::string cwd,
                       const std::string& name = {},
                       std::optional<bool> visible = std::nullopt) {
    ProjectFixture out;
    out.cwd = std::move(cwd);
    out.hash = acecode::compute_cwd_hash(out.cwd);
    out.path = root.path() / out.hash;
    fs::create_directories(out.path);
    if (visible.has_value()) {
        std::ofstream marker(out.path / "workspace.json", std::ios::binary);
        marker << nlohmann::json{
            {"cwd", out.cwd},
            {"name", name},
            {"desktop_visible", *visible},
        }.dump();
    }
    return out;
}

acecode::SessionMeta seed_meta(const ProjectFixture& project,
                               std::string id,
                               std::string updated_at,
                               bool archived = false,
                               std::string parent_session_id = {},
                               bool no_workspace = false) {
    acecode::SessionMeta meta;
    meta.id = std::move(id);
    meta.cwd = project.cwd;
    meta.created_at = updated_at;
    meta.updated_at = std::move(updated_at);
    meta.title = "title-" + meta.id;
    meta.summary = "summary-" + meta.id;
    meta.archived = archived;
    meta.parent_session_id = std::move(parent_session_id);
    meta.no_workspace = no_workspace;
    EXPECT_TRUE(acecode::SessionStorage::write_meta(
        acecode::SessionStorage::meta_path(project.path.string(), meta.id),
        meta));
    return meta;
}

const acecode::GlobalSessionCatalogEntry* find_entry(
    const acecode::GlobalSessionCatalog& catalog,
    const std::string& id,
    const std::string& workspace_hash = {}) {
    for (const auto& entry : catalog.entries) {
        if (entry.meta.id != id) continue;
        if (!workspace_hash.empty() && entry.workspace_hash != workspace_hash) continue;
        return &entry;
    }
    return nullptr;
}

acecode::ChatMessage user_message(std::string text) {
    acecode::ChatMessage message;
    message.role = "user";
    message.content = std::move(text);
    return message;
}

} // namespace

TEST(GlobalSessionCatalog, DiscoversSessionsOutsideVisibleWorkspaceRegistry) {
    TempProjectsRoot root;
    const auto visible = project(root, "C:/projects/visible", "Visible", true);
    const auto hidden = project(root, "C:/projects/hidden", "Hidden custom", false);
    const auto markerless = project(root, "C:/projects/legacy");

    seed_meta(visible, "visible-session", "2026-08-20T01:00:00Z");
    seed_meta(hidden, "hidden-session", "2026-08-20T02:00:00Z");
    seed_meta(markerless, "legacy-session", "2026-08-20T03:00:00Z");
    seed_meta(hidden, "archived-session", "2026-08-20T04:00:00Z", true);
    seed_meta(hidden, "child-session", "2026-08-20T05:00:00Z", false, "parent");
    seed_meta(markerless, "task-session", "2026-08-20T06:00:00Z", false, {}, true);

    const auto catalog = acecode::build_global_session_catalog(root.path().string());

    ASSERT_EQ(catalog.entries.size(), 4u);
    const auto* visible_entry = find_entry(catalog, "visible-session");
    ASSERT_NE(visible_entry, nullptr);
    EXPECT_TRUE(visible_entry->workspace_visible);
    EXPECT_EQ(visible_entry->workspace_name, "Visible");

    const auto* hidden_entry = find_entry(catalog, "hidden-session");
    ASSERT_NE(hidden_entry, nullptr);
    EXPECT_FALSE(hidden_entry->workspace_visible);
    EXPECT_EQ(hidden_entry->workspace_hash, hidden.hash);
    EXPECT_EQ(hidden_entry->workspace_name, "Hidden custom");
    EXPECT_EQ(hidden_entry->workspace_cwd, hidden.cwd);

    const auto* legacy_entry = find_entry(catalog, "legacy-session");
    ASSERT_NE(legacy_entry, nullptr);
    EXPECT_FALSE(legacy_entry->workspace_visible);
    EXPECT_EQ(legacy_entry->workspace_hash, markerless.hash);
    EXPECT_EQ(legacy_entry->workspace_name, "legacy");

    const auto* task_entry = find_entry(catalog, "task-session");
    ASSERT_NE(task_entry, nullptr);
    EXPECT_TRUE(task_entry->meta.no_workspace);
    EXPECT_TRUE(task_entry->workspace_hash.empty());
    EXPECT_TRUE(task_entry->workspace_name.empty());

    EXPECT_EQ(find_entry(catalog, "archived-session"), nullptr);
    EXPECT_EQ(find_entry(catalog, "child-session"), nullptr);
}

TEST(GlobalSessionCatalog, MergesActiveSessionsAndKeepsArchiveAuthoritative) {
    TempProjectsRoot root;
    const auto first = project(root, "C:/projects/first", "First", false);
    const auto second = project(root, "C:/projects/second", "Second", true);

    seed_meta(first, "same-id", "2026-08-20T01:00:00Z");
    seed_meta(second, "same-id", "2026-08-20T02:00:00Z");
    seed_meta(first, "active-persisted", "2026-08-20T03:00:00Z");
    seed_meta(first, "archived-live", "2026-08-20T04:00:00Z", true);

    acecode::SessionInfo persisted_active;
    persisted_active.id = "active-persisted";
    persisted_active.cwd = first.cwd;
    persisted_active.workspace_hash = first.hash;
    persisted_active.updated_at = "2026-08-20T05:00:00Z";
    persisted_active.title = "live title";
    persisted_active.active = true;

    acecode::SessionInfo active_only;
    active_only.id = "active-only";
    active_only.cwd = second.cwd;
    active_only.workspace_hash = second.hash;
    active_only.created_at = "2026-08-20T06:00:00Z";
    active_only.updated_at = active_only.created_at;
    active_only.title = "active only";
    active_only.active = true;

    acecode::SessionInfo archived_active = persisted_active;
    archived_active.id = "archived-live";

    const auto catalog = acecode::build_global_session_catalog(
        root.path().string(),
        {persisted_active, active_only, archived_active});

    int duplicate_count = 0;
    for (const auto& entry : catalog.entries) {
        if (entry.meta.id == "same-id") duplicate_count++;
    }
    EXPECT_EQ(duplicate_count, 2) << "same id in different workspaces stays independent";

    const auto* merged = find_entry(catalog, "active-persisted", first.hash);
    ASSERT_NE(merged, nullptr);
    ASSERT_TRUE(merged->active.has_value());
    EXPECT_EQ(merged->active->title, "live title");

    const auto* live_only = find_entry(catalog, "active-only", second.hash);
    ASSERT_NE(live_only, nullptr);
    EXPECT_TRUE(live_only->active.has_value());
    EXPECT_TRUE(live_only->workspace_visible);

    EXPECT_EQ(find_entry(catalog, "archived-live", first.hash), nullptr);
}

TEST(GlobalSessionCatalog, SearchesVisibleUserMessagesAcrossHiddenProjects) {
    TempProjectsRoot root;
    const auto hidden = project(root, "C:/projects/content-hidden", "Hidden", false);
    const auto visible = project(root, "C:/projects/content-visible", "Visible", true);
    const auto hidden_meta = seed_meta(
        hidden, "hidden-content", "2026-08-20T01:00:00Z");
    const auto visible_meta = seed_meta(
        visible, "visible-content", "2026-08-20T02:00:00Z");

    acecode::SessionStorage::write_messages(
        acecode::SessionStorage::session_path(hidden.path.string(), hidden_meta.id),
        {user_message("only the hidden project has global-needle")});
    acecode::SessionStorage::write_messages(
        acecode::SessionStorage::session_path(visible.path.string(), visible_meta.id),
        {user_message("ordinary visible content")});

    acecode::GlobalSessionCatalogOptions options;
    options.content_query = "global-needle";
    options.content_limit_per_project = 10;
    options.max_workers = 2;
    const auto catalog = acecode::build_global_session_catalog(
        root.path().string(), {}, options);

    const auto* hidden_entry = find_entry(catalog, hidden_meta.id, hidden.hash);
    ASSERT_NE(hidden_entry, nullptr);
    ASSERT_TRUE(hidden_entry->content_match.has_value());
    EXPECT_NE(hidden_entry->content_match->snippet.find("global-needle"), std::string::npos);
    EXPECT_FALSE(hidden_entry->workspace_visible);

    const auto* visible_entry = find_entry(catalog, visible_meta.id, visible.hash);
    ASSERT_NE(visible_entry, nullptr);
    EXPECT_FALSE(visible_entry->content_match.has_value());
}

TEST(GlobalSessionCatalog, NoWorkspaceDuplicateIdsMergeDeterministically) {
    TempProjectsRoot root;
    const auto older = project(root, "C:/tasks/older");
    const auto newer = project(root, "C:/tasks/newer");
    seed_meta(older, "same-task", "2026-08-20T01:00:00Z", false, {}, true);
    seed_meta(newer, "same-task", "2026-08-20T02:00:00Z", false, {}, true);

    const auto catalog = acecode::build_global_session_catalog(root.path().string());
    ASSERT_EQ(catalog.entries.size(), 1u);
    EXPECT_EQ(catalog.entries[0].meta.id, "same-task");
    EXPECT_EQ(catalog.entries[0].meta.cwd, newer.cwd);
}
