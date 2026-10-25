#include <gtest/gtest.h>

#include "session/global_session_catalog.hpp"
#include "session/global_session_search.hpp"
#include "session/session_load_metrics.hpp"
#include "session/session_storage.hpp"
#include "utils/cwd_hash.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <future>
#include <atomic>
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

// 写一个带一条用户消息的会话(meta + JSONL),供内容搜索用例命中。
void seed_session_with_message(const ProjectFixture& project,
                               const std::string& id,
                               const std::string& updated_at,
                               const std::string& user_text,
                               bool archived = false,
                               const std::string& parent_session_id = {}) {
    seed_meta(project, id, updated_at, archived, parent_session_id);
    acecode::SessionStorage::write_messages(
        acecode::SessionStorage::session_path(project.path.string(), id),
        {user_message(user_text)});
}

// 内容搜索的任务列表取自目录索引已发现的项目,所以要先等后台扫描完成。
void wait_for_catalog_complete(acecode::GlobalSessionSearchService& service,
                               const std::string& request_id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        if (service.search_sessions(request_id, {}, 50).progress.complete) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    ADD_FAILURE() << "timed out waiting for global-session catalog";
}

// 同一 request_id + query 复用同一个任务,反复调用直到所有项目都扫完。
acecode::GlobalSessionContentSearchPage run_content_search(
    acecode::GlobalSessionSearchService& service,
    const std::string& request_id,
    const std::string& query) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    acecode::GlobalSessionContentSearchPage page;
    do {
        page = service.search_user_messages_batch(request_id, query, 50);
        if (page.complete || page.cancelled) return page;
    } while (std::chrono::steady_clock::now() < deadline);
    ADD_FAILURE() << "timed out waiting for content search to finish";
    return page;
}

std::vector<std::string> content_result_ids(
    const acecode::GlobalSessionContentSearchPage& page) {
    std::vector<std::string> ids;
    for (const auto& entry : page.entries) ids.push_back(entry.meta.id);
    std::sort(ids.begin(), ids.end());
    return ids;
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

TEST(GlobalSessionCatalog, CanIncludeArchivedAndChildSessionsForThreadDiscovery) {
    TempProjectsRoot root;
    const auto target = project(root, "C:/projects/thread-discovery");
    seed_meta(target, "archived", "2026-09-18T01:00:00Z", true);
    seed_meta(target, "child", "2026-09-18T02:00:00Z", false, "parent");

    acecode::SessionInfo archived;
    archived.id = "archived";
    archived.cwd = target.cwd;
    archived.workspace_hash = target.hash;
    acecode::SessionInfo child = archived;
    child.id = "active-child";
    child.parent_session_id = "parent";

    acecode::GlobalSessionCatalogOptions options;
    options.include_subagents = true;
    const auto children = acecode::build_global_session_catalog(
        root.path().string(), {archived, child}, options);
    ASSERT_EQ(children.entries.size(), 2u);
    EXPECT_EQ(find_entry(children, "archived"), nullptr);
    EXPECT_NE(find_entry(children, "child"), nullptr);
    EXPECT_NE(find_entry(children, "active-child"), nullptr);

    options.include_archived = true;
    const auto all = acecode::build_global_session_catalog(
        root.path().string(), {archived, child}, options);
    ASSERT_EQ(all.entries.size(), 3u);
    const auto* archived_entry = find_entry(all, "archived");
    ASSERT_NE(archived_entry, nullptr);
    EXPECT_TRUE(archived_entry->meta.archived);
    EXPECT_TRUE(archived_entry->active.has_value());

    EXPECT_TRUE(acecode::build_global_session_catalog(
        root.path().string(), {archived, child}).entries.empty());
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

// 场景：首页任务建议按目录修改时间由新到旧查找会话，并复用后台目录索引已经扫过的项目，
// 避免每次刷新重扫全部项目目录(本机 2.4 万个，冷缓存要 30 多秒)。
// 期望：目录列表按修改时间从新到旧，带可与索引对照的签名和 Unix 毫秒时间；单项目扫描与
// 索引口径一致(不含归档会话)；只有签名与索引记录一致时才返回索引里的行，目录在索引
// 之后又被写过(签名不同)、签名未知或索引尚未扫描时都不复用。
TEST(GlobalSessionCatalogIndex, CachedProjectRowsRequireMatchingDirectorySignature) {
    TempProjectsRoot root;
    const auto older = project(root, "C:/projects/cache-older");
    const auto newer = project(root, "C:/projects/cache-newer");
    seed_meta(older, "older-session", "2026-08-23T01:00:00Z");
    seed_meta(older, "older-archived", "2026-08-23T01:30:00Z", true);
    seed_meta(newer, "newer-session", "2026-08-23T02:00:00Z");
    const auto file_now = fs::file_time_type::clock::now();
    fs::last_write_time(older.path, file_now - std::chrono::hours(2));
    fs::last_write_time(newer.path, file_now - std::chrono::hours(1));

    const auto listed = acecode::discover_global_session_projects(root.path().string());
    ASSERT_EQ(listed.size(), 2u);
    EXPECT_EQ(fs::path(listed[0].project_dir).filename().string(), newer.hash);
    EXPECT_EQ(fs::path(listed[1].project_dir).filename().string(), older.hash);
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    EXPECT_NEAR(static_cast<double>(now_ms - listed[0].modified_ms), 3600000.0, 60000.0);
    EXPECT_NEAR(static_cast<double>(now_ms - listed[1].modified_ms), 7200000.0, 60000.0);

    const auto scanned = acecode::scan_global_session_project(listed[1].project_dir);
    ASSERT_EQ(scanned.size(), 1u);
    EXPECT_EQ(scanned[0].meta.id, "older-session");

    // 逐个文件读取(大目录只读最新几份)与整目录扫描得到同样的行：归档会话同样不返回。
    const acecode::GlobalSessionProjectReader reader(listed[1].project_dir);
    const auto files = reader.list();
    ASSERT_EQ(files.size(), 2u);
    std::vector<acecode::GlobalSessionCatalogEntry> read_rows;
    for (const auto& file : files) {
        EXPECT_GT(file.modified_ms, now_ms - 600000);
        EXPECT_LE(file.modified_ms, now_ms + 1000);
        if (auto row = reader.read(file.meta_path)) read_rows.push_back(std::move(*row));
    }
    ASSERT_EQ(read_rows.size(), 1u);
    EXPECT_EQ(read_rows[0].meta.id, scanned[0].meta.id);
    EXPECT_EQ(read_rows[0].workspace_hash, scanned[0].workspace_hash);
    EXPECT_EQ(read_rows[0].workspace_cwd, scanned[0].workspace_cwd);
    EXPECT_EQ(read_rows[0].workspace_name, scanned[0].workspace_name);
    EXPECT_EQ(read_rows[0].project_dir, scanned[0].project_dir);

    acecode::GlobalSessionCatalogIndex index(root.path().string());
    EXPECT_FALSE(index.cached_project_entries(listed[1].project_dir, listed[1].signature));
    ASSERT_TRUE(index.attach_request("cache-search"));
    index.start();
    wait_for_index(index, [](const auto& snapshot) { return snapshot.progress.complete; });
    const auto cached = index.cached_project_entries(listed[1].project_dir, listed[1].signature);
    ASSERT_TRUE(cached);
    ASSERT_EQ(cached->size(), 1u);
    EXPECT_EQ((*cached)[0].meta.id, "older-session");
    EXPECT_FALSE(index.cached_project_entries(listed[1].project_dir, listed[1].signature + 1));
    EXPECT_FALSE(index.cached_project_entries(listed[1].project_dir, 0));
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

// 触发场景:Ctrl+K 全文搜索,同一项目里普通会话、归档会话、子会话的用户消息
// 都包含查询词;另有两个会话是目录索引扫描完成之后才落盘的(分片里没有它们,
// 结果要走「按元数据现拼条目」的回退路径),其中一个是归档的。
// 期望行为:只返回两个未归档的主会话;归档会话与子会话即使正文命中也被排除,
// 回退路径与目录命中路径给出相同的工作区信息。
TEST(GlobalSessionSearchService, ContentSearchExcludesArchivedAndChildSessionsMatchedByContent) {
    TempProjectsRoot root;
    const auto fixture = project(
        root, "C:/projects/content-filter", "Content Named", true);
    seed_session_with_message(fixture, "ordinary", "2026-10-01T01:00:00Z",
                              "ordinary mentions filter-needle");
    seed_session_with_message(fixture, "archived", "2026-10-01T02:00:00Z",
                              "archived mentions filter-needle", true);
    seed_session_with_message(fixture, "child", "2026-10-01T03:00:00Z",
                              "child mentions filter-needle", false, "ordinary");

    acecode::GlobalSessionSearchService service(root.path().string());
    service.start();
    wait_for_catalog_complete(service, "filter-request");

    // 扫描完成后才出现的会话:不调 invalidate_project,目录分片里查不到。
    seed_session_with_message(fixture, "late-ordinary", "2026-10-01T04:00:00Z",
                              "late ordinary mentions filter-needle");
    seed_session_with_message(fixture, "late-archived", "2026-10-01T05:00:00Z",
                              "late archived mentions filter-needle", true);

    const auto page = run_content_search(service, "filter-request", "filter-needle");
    ASSERT_TRUE(page.complete);
    EXPECT_TRUE(page.errors.empty());
    EXPECT_EQ(content_result_ids(page),
              (std::vector<std::string>{"late-ordinary", "ordinary"}));
    for (const auto& entry : page.entries) {
        ASSERT_TRUE(entry.content_match.has_value()) << entry.meta.id;
        EXPECT_EQ(entry.workspace_hash, fixture.hash) << entry.meta.id;
        EXPECT_EQ(entry.workspace_name, "Content Named") << entry.meta.id;
        EXPECT_FALSE(entry.meta.archived) << entry.meta.id;
        EXPECT_TRUE(entry.meta.parent_session_id.empty()) << entry.meta.id;
    }
    service.stop();
}

// 触发场景:目录索引扫描时会话还是普通会话,之后在磁盘上被归档(别的进程归档、
// 或搜索面板关闭期间后台扫描暂停),目录分片还没刷新。
// 期望行为:元数据搜索仍按旧分片返回它(这是分片的既有延迟,用作本用例的前提),
// 但内容搜索以磁盘上的最新元数据为准把它排除 —— 与改动前「每个项目重新枚举
// 目录再过滤」的语义一致。
// 回归:若改成「目录分片里找得到就直接采信」,刚归档的会话会漏进全文结果。
TEST(GlobalSessionSearchService, ContentSearchUsesDiskArchiveStateWhenCatalogShardIsStale) {
    TempProjectsRoot root;
    const auto fixture = project(root, "C:/projects/content-stale");
    seed_session_with_message(fixture, "keep", "2026-10-02T01:00:00Z",
                              "keep mentions stale-needle");
    seed_session_with_message(fixture, "flip", "2026-10-02T02:00:00Z",
                              "flip mentions stale-needle");

    acecode::GlobalSessionSearchService service(root.path().string());
    service.start();
    wait_for_catalog_complete(service, "stale-request");

    // 只改磁盘上的 meta,不通知目录索引。
    seed_meta(fixture, "flip", "2026-10-02T02:00:00Z", /*archived=*/true);
    const auto metadata = service.search_sessions("stale-request", "title-flip", 10);
    bool stale_shard_still_lists_flip = false;
    for (const auto& entry : metadata.entries) {
        if (entry.meta.id == "flip") stale_shard_still_lists_flip = true;
    }
    ASSERT_TRUE(stale_shard_still_lists_flip)
        << "前提不成立:目录分片已经刷新,本用例测不到分片滞后的场景";

    const auto page = run_content_search(service, "stale-request", "stale-needle");
    ASSERT_TRUE(page.complete);
    EXPECT_EQ(content_result_ids(page), (std::vector<std::string>{"keep"}));
    service.stop();
}

// 触发场景:索引已是热的,全文搜索扫过两个项目 —— A 有 10 个会话、其中 1 个
// 命中,B 有 6 个会话、无命中。
// 期望行为:每个项目的 .meta.json 只被打开一次(刷新内容索引时那一遍),总数
// 不超过 16。Windows 上会话列表的成本在文件个数,2.8 万会话实测一遍约 3.3 秒。
// 回归:改动前刷新索引读一遍、过滤命中又整目录读一遍,共 32 次;只做「无命中
// 跳过第二遍」也还有 26 次,两种都会被这里拦下。
TEST(GlobalSessionSearchService, ContentSearchOpensEachMetaFileOncePerProject) {
    constexpr int kMatchingProjectSessions = 10;
    constexpr int kQuietProjectSessions = 6;
    TempProjectsRoot root;
    const auto matching = project(root, "C:/projects/content-io-matching");
    const auto quiet = project(root, "C:/projects/content-io-quiet");
    for (int i = 0; i < kMatchingProjectSessions; ++i) {
        seed_session_with_message(
            matching, "matching-" + std::to_string(i), "2026-10-03T01:00:00Z",
            i == 0 ? "only this one has io-needle" : "unrelated text");
    }
    for (int i = 0; i < kQuietProjectSessions; ++i) {
        seed_session_with_message(
            quiet, "quiet-" + std::to_string(i), "2026-10-03T02:00:00Z",
            "unrelated text");
    }

    acecode::GlobalSessionSearchService service(root.path().string());
    service.start();
    wait_for_catalog_complete(service, "io-warmup");
    // 先跑一遍把内容索引建好;建索引要读 JSONL,不在本用例的计数范围内。
    ASSERT_TRUE(run_content_search(service, "io-warmup", "io-needle").complete);

    // 计数器是线程局部的,后台目录扫描线程的读取不会混进来。
    const auto before = acecode::session_read_metrics();
    const auto page = run_content_search(service, "io-measured", "io-needle");
    const auto opened = (acecode::session_read_metrics() - before).files;

    ASSERT_TRUE(page.complete);
    EXPECT_EQ(content_result_ids(page), (std::vector<std::string>{"matching-0"}));
    EXPECT_LE(opened, static_cast<std::uint64_t>(
        kMatchingProjectSessions + kQuietProjectSessions));
    service.stop();
}

TEST(GlobalSessionCatalogIndex, WarmupWaitsForInteractionOrSearchSignal) {
    for (const bool search : {false, true}) {
        TempProjectsRoot root;
        const auto fixture = project(root, "C:/projects/deferred");
        seed_meta(fixture, "deferred", "2026-10-01T00:00:00Z");
        struct ClockState {
            std::atomic<int> calls{0};
            std::promise<void> waiting;
        };
        auto clock = std::make_shared<ClockState>(); // Test and worker share only the clock probe.
        auto waiting = clock->waiting.get_future();
        acecode::GlobalSessionCatalogIndex index(root.path().string(), {}, [clock] {
            if (clock->calls.fetch_add(1) == 1) clock->waiting.set_value();
            return std::chrono::steady_clock::time_point{};
        });
        index.start();
        ASSERT_EQ(waiting.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        EXPECT_EQ(index.snapshot().progress.scanned_projects, 0u);
        EXPECT_FALSE(index.snapshot().progress.complete);
        if (search) EXPECT_TRUE(index.attach_request("early-search"));
        else index.notify_startup_interaction();
        const auto result = wait_for_index(index, [](const auto& snapshot) { return snapshot.progress.complete; });
        EXPECT_TRUE(result.progress.complete);
        EXPECT_EQ(result.catalog.entries.size(), 1u);
        index.stop();
    }
}

TEST(GlobalSessionCatalogIndex, WarmupUsesInjectedTenSecondDeadlineAndStopsWhileWaiting) {
    TempProjectsRoot root;
    const auto fixture = project(root, "C:/projects/deadline");
    seed_meta(fixture, "deadline", "2026-10-01T00:00:00Z");
    auto calls = std::make_shared<std::atomic<int>>(0); // Shared clock state, no borrowed worker capture.
    acecode::GlobalSessionCatalogIndex index(root.path().string(), {}, [calls] {
        return std::chrono::steady_clock::time_point{} +
               std::chrono::seconds(calls->fetch_add(1) == 0 ? 0 : 10);
    });
    index.start();
    EXPECT_TRUE(wait_for_index(index, [](const auto& snapshot) { return snapshot.progress.complete; }).progress.complete);
    index.stop();
    acecode::GlobalSessionCatalogIndex stopped(root.path().string(), {}, [] {
        return std::chrono::steady_clock::time_point{};
    });
    stopped.start();
    const auto before = std::chrono::steady_clock::now();
    stopped.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::seconds(1));
}
