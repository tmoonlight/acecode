#include <gtest/gtest.h>

#include "remote_control/rc_session_catalog.hpp"
#include "remote_control/rc_session_navigation.hpp"
#include "session/session_registry.hpp"
#include "session/session_storage.hpp"
#include "utils/paths.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

using acecode::rc::RcSessionCatalogEntry;
using acecode::rc::RcSessionCatalog;
using acecode::rc::RcSessionCatalogDeps;
using acecode::rc::RcSessionCommandKind;
using acecode::rc::format_rc_session_list;
using acecode::rc::parse_rc_session_command;
using acecode::rc::rc_session_entry_key;
using acecode::rc::search_rc_sessions;
using acecode::rc::sort_rc_sessions_newest_first;

namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
constexpr const char* kCatalogHomeEnv = "USERPROFILE";
#else
constexpr const char* kCatalogHomeEnv = "HOME";
#endif

class ScopedCatalogHome {
public:
    explicit ScopedCatalogHome(const fs::path& home) {
        if (const char* current = std::getenv(kCatalogHomeEnv)) {
            had_old_ = true;
            old_ = current;
        }
#ifdef _WIN32
        _putenv_s(kCatalogHomeEnv, home.string().c_str());
#else
        setenv(kCatalogHomeEnv, home.string().c_str(), 1);
#endif
    }

    ~ScopedCatalogHome() {
#ifdef _WIN32
        _putenv_s(kCatalogHomeEnv, had_old_ ? old_.c_str() : "");
#else
        if (had_old_) setenv(kCatalogHomeEnv, old_.c_str(), 1);
        else unsetenv(kCatalogHomeEnv);
#endif
    }

private:
    bool had_old_ = false;
    std::string old_;
};

fs::path catalog_root(const std::string& tag) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    auto root = fs::temp_directory_path() /
                ("acecode-rc-catalog-" + tag + "-" + std::to_string(tick));
    fs::create_directories(root / "home");
    return root;
}

void write_workspace(const fs::path& project_dir,
                     const std::string& cwd,
                     const std::string& name) {
    fs::create_directories(project_dir);
    std::ofstream out(project_dir / "workspace.json", std::ios::binary);
    out << nlohmann::json{{"cwd", cwd}, {"name", name}}.dump();
}

void write_session(const fs::path& project_dir,
                   const std::string& id,
                   const std::string& title,
                   const std::string& updated_at,
                   bool archived = false,
                   bool no_workspace = false,
                   const std::string& parent_session_id = {},
                   const std::string& cwd = {}) {
    acecode::SessionMeta meta;
    meta.id = id;
    meta.cwd = cwd.empty() ? project_dir.string() : cwd;
    meta.title = title;
    meta.created_at = updated_at;
    meta.updated_at = updated_at;
    meta.archived = archived;
    meta.no_workspace = no_workspace;
    meta.parent_session_id = parent_session_id;
    ASSERT_TRUE(acecode::SessionStorage::write_meta(
        acecode::SessionStorage::meta_path(project_dir.string(), id), meta));
}

RcSessionCatalogEntry entry(std::string id,
                            std::string title,
                            std::string updated,
                            std::string workspace = "workspace") {
    RcSessionCatalogEntry value;
    value.id = std::move(id);
    value.title = std::move(title);
    value.updated_at = std::move(updated);
    value.workspace_hash = workspace;
    value.workspace_name = workspace;
    return value;
}

} // namespace

TEST(RcSessionCommand, AliasesAreCaseInsensitiveAndEquivalent) {
    for (const auto* text : {"/session", "/sessions", "/resume",
                             " /SESSION ", "/ReSuMe"}) {
        const auto parsed = parse_rc_session_command(text);
        EXPECT_EQ(parsed.kind, RcSessionCommandKind::ListRecent) << text;
    }
    EXPECT_EQ(parse_rc_session_command("/session all").kind,
              RcSessionCommandKind::ListAll);
    EXPECT_EQ(parse_rc_session_command("/sessions MORE").kind,
              RcSessionCommandKind::ListAll);
}

TEST(RcSessionCommand, SearchAndSelectionGrammar) {
    const auto search = parse_rc_session_command("/resume search login failure");
    EXPECT_EQ(search.kind, RcSessionCommandKind::Search);
    EXPECT_EQ(search.query, "login failure");

    const auto select = parse_rc_session_command("/sessions 3");
    EXPECT_EQ(select.kind, RcSessionCommandKind::Select);
    EXPECT_EQ(select.number, 3u);

    EXPECT_EQ(parse_rc_session_command("/sessions search").kind,
              RcSessionCommandKind::Invalid);
    EXPECT_EQ(parse_rc_session_command("/sessions 0").kind,
              RcSessionCommandKind::Invalid);
    EXPECT_EQ(parse_rc_session_command("/sessions nope").kind,
              RcSessionCommandKind::Invalid);
    EXPECT_EQ(parse_rc_session_command("/rc").kind,
              RcSessionCommandKind::NotCommand);
}

TEST(RcSessionCatalogLogic, SortsNewestWithDeterministicTieBreak) {
    auto sorted = sort_rc_sessions_newest_first({
        entry("b", "B", "2026-08-05T01:00:00Z"),
        entry("c", "C", "2026-08-05T02:00:00Z"),
        entry("a", "A", "2026-08-05T01:00:00Z"),
    });
    ASSERT_EQ(sorted.size(), 3u);
    EXPECT_EQ(sorted[0].id, "c");
    EXPECT_EQ(sorted[1].id, "a");
    EXPECT_EQ(sorted[2].id, "b");
}

TEST(RcSessionCatalogLogic, SearchUsesMetadataAndContentAndCapsAtFive) {
    std::vector<RcSessionCatalogEntry> entries;
    for (int i = 0; i < 8; ++i) {
        auto value = entry("s" + std::to_string(i),
                           i == 0 ? "needle title" : "other",
                           "2026-08-05T0" + std::to_string(i) + ":00:00Z");
        entries.push_back(std::move(value));
    }
    std::unordered_map<std::string, int> content;
    for (std::size_t i = 1; i < entries.size(); ++i) {
        content[rc_session_entry_key(entries[i])] = 500 + static_cast<int>(i);
    }
    auto result = search_rc_sessions(entries, "needle", content, 5);
    ASSERT_EQ(result.size(), 5u);
    EXPECT_EQ(result.front().id, "s0");
}

TEST(RcSessionCatalogLogic, FormattingKeepsContinuousNumbersAcrossChunks) {
    std::vector<RcSessionCatalogEntry> entries;
    for (int i = 0; i < 12; ++i) {
        entries.push_back(entry("s" + std::to_string(i),
                                "a deliberately long session title " + std::to_string(i),
                                "2026-08-05T12:34:56Z"));
    }
    const auto chunks = format_rc_session_list(entries, "全部会话", 256);
    ASSERT_GT(chunks.size(), 1u);
    std::string joined;
    for (const auto& chunk : chunks) joined += "\n" + chunk;
    for (int i = 1; i <= 12; ++i) {
        EXPECT_NE(joined.find("\n" + std::to_string(i) + ". "), std::string::npos);
    }
}

TEST(RcSessionCatalog, EnumeratesEveryPersistedWorkspaceAndNoWorkspaceSession) {
    const auto root = catalog_root("persisted");
    {
        ScopedCatalogHome home(root / "home");
        const fs::path projects = root / "home" / ".acecode" / "projects";
        const fs::path workspace_a = projects / "workspace-a";
        const fs::path workspace_b = projects / "workspace-b";
        write_workspace(workspace_a, (root / "work-a").string(), "Alpha");
        write_workspace(workspace_b, (root / "work-b").string(), "Beta");
        write_session(workspace_a, "ordinary-a", "Alpha task",
                      "2026-08-05T03:00:00Z", false, false, {},
                      (root / "work-a").string());
        write_session(workspace_b, "ordinary-b", "Beta task",
                      "2026-08-05T02:00:00Z", false, false, {},
                      (root / "work-b").string());
        write_session(workspace_a, "archived", "Archived task",
                      "2026-08-05T05:00:00Z", true);
        write_session(workspace_b, "child", "Background child",
                      "2026-08-05T04:00:00Z", false, false, "parent");

        const fs::path no_workspace_root =
            root / "home" / ".acecode" / "cache" / "no-workspace";
        const fs::path no_workspace_cwd = no_workspace_root / "ordinary-no-ws";
        fs::create_directories(no_workspace_cwd);
        const fs::path no_workspace_project =
            acecode::SessionStorage::get_project_dir(no_workspace_cwd.string());
        write_session(no_workspace_project, "ordinary-no-ws", "Loose task",
                      "2026-08-05T01:00:00Z", false, true, {},
                      no_workspace_cwd.string());

        RcSessionCatalog catalog(RcSessionCatalogDeps{
            projects.string(),
            no_workspace_root.string(),
            [] { return std::vector<acecode::SessionInfo>{}; },
            {},
        });
        const auto sessions = catalog.list_all();
        ASSERT_EQ(sessions.size(), 3u);
        EXPECT_EQ(sessions[0].id, "ordinary-a");
        EXPECT_EQ(sessions[1].id, "ordinary-b");
        EXPECT_EQ(sessions[2].id, "ordinary-no-ws");
        EXPECT_TRUE(sessions[2].no_workspace);
        EXPECT_TRUE(sessions[2].workspace_hash.empty());
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(RcSessionCatalog, SearchFallsBackToMetadataWhenContentIndexFails) {
    acecode::SessionInfo active;
    active.id = "active-session";
    active.cwd = "C:/work/example";
    active.workspace_hash = "workspace-example";
    active.title = "Login investigation";
    active.updated_at = "2026-08-05T03:00:00Z";
    active.active = true;

    RcSessionCatalog catalog(RcSessionCatalogDeps{
        "Z:/missing/projects",
        "Z:/missing/no-workspace",
        [active] { return std::vector<acecode::SessionInfo>{active}; },
        [](const std::string&, const std::string&)
            -> std::unordered_map<std::string, int> {
            throw std::runtime_error("index unavailable");
        },
    });
    const auto result = catalog.search("login", 5);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.front().id, "active-session");
}
