#include <gtest/gtest.h>

#include "session/global_session_catalog.hpp"
#include "session/global_session_search.hpp"
#include "session/session_storage.hpp"
#include "utils/cwd_hash.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <thread>
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

acecode::GlobalSessionCatalogIndexSnapshot wait_for_index(
    acecode::GlobalSessionCatalogIndex& index,
    const std::function<bool(const acecode::GlobalSessionCatalogIndexSnapshot&)>& predicate,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        auto snapshot = index.snapshot();
        if (predicate(snapshot)) return snapshot;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    ADD_FAILURE() << "timed out waiting for incremental global-session index";
    return index.snapshot();
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

TEST(GlobalSessionCatalogIndex, CancelsPrewarmAndResumesCommittedShards) {
    TempProjectsRoot root;
    constexpr int kProjects = 40;
    for (int i = 0; i < kProjects; ++i) {
        const auto fixture = project(
            root, "C:/projects/index-" + std::to_string(i));
        seed_meta(
            fixture,
            "session-" + std::to_string(i),
            "2026-08-23T00:00:" + std::to_string(10 + (i % 40)) + "Z");
    }

    acecode::GlobalSessionCatalogIndex index(root.path().string());
    ASSERT_TRUE(index.attach_request("first-search"));
    index.start();
    const auto partial = wait_for_index(index, [=](const auto& snapshot) {
        return snapshot.progress.total_projects == kProjects &&
            snapshot.progress.scanned_projects >= 3 &&
            !snapshot.progress.complete;
    });
    ASSERT_LT(partial.progress.scanned_projects, partial.progress.total_projects);

    EXPECT_TRUE(index.cancel_request("first-search"));
    EXPECT_FALSE(index.cancel_request("first-search"));
    const auto paused = wait_for_index(index, [](const auto& snapshot) {
        return snapshot.progress.paused;
    });
    const auto committed_before_wait = paused.progress.scanned_projects;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto still_paused = index.snapshot();
    EXPECT_TRUE(still_paused.progress.paused);
    EXPECT_EQ(still_paused.progress.scanned_projects, committed_before_wait);

    // Legacy clients omit request_id. They cannot explicitly detach, but a
    // request must still resume a worker paused by a newer cancellable client.
    ASSERT_TRUE(index.attach_request(""));
    const auto legacy_resumed = wait_for_index(index, [&](const auto& snapshot) {
        return !snapshot.progress.paused &&
            snapshot.progress.scanned_projects > committed_before_wait;
    });
    EXPECT_GT(legacy_resumed.progress.scanned_projects, committed_before_wait);

    ASSERT_TRUE(index.attach_request("second-search"));
    const auto complete = wait_for_index(index, [](const auto& snapshot) {
        return snapshot.progress.complete;
    });
    EXPECT_EQ(complete.progress.scanned_projects, kProjects);
    EXPECT_EQ(complete.catalog.entries.size(), kProjects);
    index.stop();
}

TEST(GlobalSessionCatalogIndex, RefreshesOnlyInvalidatedProjectShard) {
    TempProjectsRoot root;
    const auto first = project(root, "C:/projects/index-refresh-first");
    const auto second = project(root, "C:/projects/index-refresh-second");
    seed_meta(first, "first-old", "2026-08-23T01:00:00Z");
    seed_meta(second, "second-stable", "2026-08-23T02:00:00Z");

    acecode::GlobalSessionCatalogIndex index(root.path().string());
    ASSERT_TRUE(index.attach_request("refresh-search"));
    index.start();
    const auto before = wait_for_index(index, [](const auto& snapshot) {
        return snapshot.progress.complete;
    });
    ASSERT_NE(find_entry(before.catalog, "first-old", first.hash), nullptr);
    ASSERT_NE(find_entry(before.catalog, "second-stable", second.hash), nullptr);

    seed_meta(first, "first-new", "2026-08-23T03:00:00Z");
    index.invalidate_project(first.hash);
    const auto refreshed = wait_for_index(index, [&](const auto& snapshot) {
        return snapshot.progress.generation > before.progress.generation &&
            find_entry(snapshot.catalog, "first-new", first.hash) != nullptr;
    });
    EXPECT_NE(find_entry(refreshed.catalog, "first-old", first.hash), nullptr);
    EXPECT_NE(find_entry(refreshed.catalog, "second-stable", second.hash), nullptr);
    EXPECT_EQ(refreshed.progress.scanned_projects, 2u);
    index.stop();
}

TEST(GlobalSessionCatalogIndex, PreservesGlobalFilteringAndActiveMerge) {
    TempProjectsRoot root;
    const auto hidden = project(root, "C:/projects/index-hidden", "Hidden", false);
    seed_meta(hidden, "ordinary", "2026-08-23T01:00:00Z");
    seed_meta(hidden, "archived", "2026-08-23T02:00:00Z", true);
    seed_meta(hidden, "child", "2026-08-23T03:00:00Z", false, "parent");
    seed_meta(hidden, "task", "2026-08-23T04:00:00Z", false, {}, true);

    acecode::SessionInfo active;
    active.id = "active-only";
    active.cwd = hidden.cwd;
    active.workspace_hash = hidden.hash;
    active.created_at = "2026-08-23T05:00:00Z";
    active.updated_at = active.created_at;

    acecode::GlobalSessionCatalogIndex index(
        root.path().string(), [active] { return std::vector<acecode::SessionInfo>{active}; });
    ASSERT_TRUE(index.attach_request("filter-search"));
    index.start();
    const auto snapshot = wait_for_index(index, [](const auto& value) {
        return value.progress.complete;
    });
    EXPECT_NE(find_entry(snapshot.catalog, "ordinary", hidden.hash), nullptr);
    EXPECT_NE(find_entry(snapshot.catalog, "task"), nullptr);
    EXPECT_NE(find_entry(snapshot.catalog, "active-only", hidden.hash), nullptr);
    EXPECT_EQ(find_entry(snapshot.catalog, "archived", hidden.hash), nullptr);
    EXPECT_EQ(find_entry(snapshot.catalog, "child", hidden.hash), nullptr);
    index.stop();
}

TEST(GlobalSessionSearchService, BoundsPagesAndInvalidatesGenerationCursor) {
    TempProjectsRoot root;
    std::vector<ProjectFixture> projects;
    for (int i = 0; i < 65; ++i) {
        projects.push_back(project(
            root, "C:/projects/search-page-" + std::to_string(i)));
        seed_meta(
            projects.back(),
            "page-session-" + std::to_string(i),
            "2026-08-23T00:00:" + std::to_string(100 + i) + "Z");
    }

    acecode::GlobalSessionSearchService service(root.path().string());
    service.start();
    acecode::GlobalSessionSearchPage first;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    do {
        first = service.search_sessions("page-request", {}, 100);
        if (first.progress.complete) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);

    ASSERT_TRUE(first.progress.complete);
    ASSERT_EQ(first.entries.size(), 50u);
    ASSERT_TRUE(first.next_cursor.has_value());
    const auto second = service.search_sessions(
        "page-request", {}, 50, *first.next_cursor);
    ASSERT_FALSE(second.cursor_stale);
    EXPECT_EQ(second.entries.size(), 15u);
    EXPECT_FALSE(second.next_cursor.has_value());

    seed_meta(
        projects.front(), "page-new", "2026-08-23T09:00:00Z");
    service.invalidate_project(projects.front().hash);
    acecode::GlobalSessionSearchPage stale;
    do {
        stale = service.search_sessions(
            "page-request", {}, 50, *first.next_cursor);
        if (stale.cursor_stale) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    EXPECT_TRUE(stale.cursor_stale);
    service.stop();
}

TEST(GlobalSessionSearchService, ContentBatchesStopAfterCancellation) {
    TempProjectsRoot root;
    constexpr int kProjects = 40;
    for (int i = 0; i < kProjects; ++i) {
        const auto fixture = project(
            root, "C:/projects/content-batch-" + std::to_string(i));
        const auto id = "content-session-" + std::to_string(i);
        seed_meta(fixture, id, "2026-08-23T01:00:00Z");
        acecode::SessionStorage::write_messages(
            acecode::SessionStorage::session_path(fixture.path.string(), id),
            {user_message("interruptible-content-needle")});
    }

    acecode::GlobalSessionSearchService service(root.path().string());
    service.start();
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto metadata = service.search_sessions(
            "content-request", {}, 50);
        if (metadata.progress.complete) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto partial = service.search_user_messages_batch(
        "content-request", "interruptible-content-needle", 50);
    EXPECT_GT(partial.scanned_projects, 0u);
    EXPECT_LT(partial.scanned_projects, partial.total_projects);
    EXPECT_FALSE(partial.complete);
    EXPECT_TRUE(service.cancel("content-request"));
    EXPECT_FALSE(service.cancel("content-request"));

    const auto cancelled = service.search_user_messages_batch(
        "content-request", "interruptible-content-needle", 50);
    EXPECT_TRUE(cancelled.cancelled);
    EXPECT_EQ(cancelled.scanned_projects, 0u);
    service.stop();
}
