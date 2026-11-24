#include <gtest/gtest.h>

#include "session/archived_session_index.hpp"
#include "session/session_load_metrics.hpp"
#include "session/session_storage.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using acecode::SessionMeta;
using acecode::SessionStorage;

namespace {

// 每个用例一个独立的数据根:<tmp>/projects/<目录名>/ 是索引认的标准布局,
// 索引文件落在 <tmp>/archived-sessions.json。
fs::path make_data_root(const std::string& hint) {
    auto base = fs::temp_directory_path() /
                ("acecode_archived_index_" + hint + "_" +
                 std::to_string(::testing::UnitTest::GetInstance()->current_test_info()->line()));
    fs::remove_all(base);
    fs::create_directories(base / "projects");
    return base;
}

fs::path make_project(const fs::path& root, const std::string& name) {
    const auto dir = root / "projects" / name;
    fs::create_directories(dir);
    return dir;
}

SessionMeta make_meta(const std::string& id, bool archived, bool no_workspace = false) {
    SessionMeta meta;
    meta.id = id;
    meta.cwd = "/tmp/x";
    meta.created_at = "2026-01-01T00:00:00Z";
    meta.updated_at = "2026-01-01T00:00:00Z";
    meta.message_count = 1;
    meta.provider = "openai";
    meta.model = "test";
    meta.archived = archived;
    meta.no_workspace = no_workspace;
    return meta;
}

// 正常写入:经 SessionStorage::write_meta,归档时会登记进索引。
void write_session(const fs::path& dir, const std::string& id, bool archived,
                   bool no_workspace = false) {
    ASSERT_TRUE(SessionStorage::write_meta(
        SessionStorage::meta_path(dir.string(), id), make_meta(id, archived, no_workspace)));
}

// 模拟升级前旧版本写下的 meta:直接写文件,不经过登记。
void write_legacy_session(const fs::path& dir, const std::string& id, bool archived,
                          bool no_workspace = false) {
    nlohmann::json j = {{"id", id}, {"cwd", "/tmp/x"},
                        {"created_at", "2026-01-01T00:00:00Z"},
                        {"updated_at", "2026-01-01T00:00:00Z"},
                        {"message_count", 1}, {"provider", "openai"}, {"model", "test"}};
    if (archived) j["archived"] = true;
    if (no_workspace) j["no_workspace"] = true;
    std::ofstream(dir / (id + ".meta.json"), std::ios::binary) << j.dump(2);
}

nlohmann::json read_index(const fs::path& root) {
    std::ifstream in(root / "archived-sessions.json", std::ios::binary);
    if (!in) return nullptr;
    return nlohmann::json::parse(in, nullptr, false);
}

std::vector<std::string> sorted_ids(const std::vector<SessionMeta>& metas) {
    std::vector<std::string> ids;
    for (const auto& meta : metas) ids.push_back(meta.id);
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::uint64_t opened_files() {
    return acecode::session_read_metrics().files;
}

} // namespace

// 场景:升级前的数据——目录里 40 个未归档会话、2 个旧版本归档的会话,索引还不存在。
// 期望:第一次列出整目录扫一遍,找回这 2 个并把目录标记为已扫描;第二次只打开索引登记
//       的 2 个 meta,不再碰另外 40 个。
// 回归:归档页每次都把整个目录读一遍,用户 5065 个会话、0 个归档的目录冷读要 23 秒。
TEST(ArchivedSessionIndex, ScansDirectoryOnceThenOpensOnlyIndexedSessions) {
    const auto root = make_data_root("scan_once");
    const auto dir = make_project(root, "aaaa000000000001");
    for (int i = 0; i < 40; ++i) write_legacy_session(dir, "live-" + std::to_string(i), false);
    write_legacy_session(dir, "old-archived-1", true);
    write_legacy_session(dir, "old-archived-2", true);

    EXPECT_EQ(sorted_ids(acecode::list_archived_session_metadata(dir.string())),
              (std::vector<std::string>{"old-archived-1", "old-archived-2"}));
    const auto index = read_index(root);
    ASSERT_TRUE(index.is_object());
    EXPECT_TRUE(index["scanned_projects"].contains("aaaa000000000001"));
    EXPECT_EQ(index["sessions"].size(), 2u);

    const auto before = opened_files();
    EXPECT_EQ(sorted_ids(acecode::list_archived_session_metadata(dir.string())),
              (std::vector<std::string>{"old-archived-1", "old-archived-2"}));
    EXPECT_EQ(opened_files() - before, 2u);
    fs::remove_all(root);
}

// 场景:目录已经扫描过,之后经 write_meta 归档一个新会话。
// 期望:不用重新扫描就能在归档列表里看到它,且列表只打开登记的那一个 meta。
TEST(ArchivedSessionIndex, ArchivingThroughWriteMetaIsListedWithoutRescan) {
    const auto root = make_data_root("write_hook");
    const auto dir = make_project(root, "aaaa000000000002");
    for (int i = 0; i < 10; ++i) write_session(dir, "live-" + std::to_string(i), false);
    EXPECT_TRUE(acecode::list_archived_session_metadata(dir.string()).empty());

    write_session(dir, "live-3", true);

    const auto before = opened_files();
    EXPECT_EQ(sorted_ids(acecode::list_archived_session_metadata(dir.string())),
              (std::vector<std::string>{"live-3"}));
    EXPECT_EQ(opened_files() - before, 1u);
    fs::remove_all(root);
}

// 场景:两个已归档会话,一个被取消归档(meta 重写为 archived=false),一个被永久删除。
// 期望:两者都不再出现在归档列表里,并且从索引中剔除,之后的列表不再为它们打开文件。
TEST(ArchivedSessionIndex, UnarchivedAndDeletedSessionsArePruned) {
    const auto root = make_data_root("prune");
    const auto dir = make_project(root, "aaaa000000000003");
    write_session(dir, "restored", true);
    write_session(dir, "purged", true);
    EXPECT_EQ(acecode::list_archived_session_metadata(dir.string()).size(), 2u);

    write_session(dir, "restored", false);
    fs::remove(dir / "purged.meta.json");

    EXPECT_TRUE(acecode::list_archived_session_metadata(dir.string()).empty());
    EXPECT_TRUE(read_index(root)["sessions"].empty());
    fs::remove_all(root);
}

// 场景:取消归档后又重新归档。
// 期望:剔除之后重新登记,归档列表里能再次看到它。
TEST(ArchivedSessionIndex, RearchivedSessionIsListedAgain) {
    const auto root = make_data_root("rearchive");
    const auto dir = make_project(root, "aaaa000000000004");
    write_session(dir, "toggle", true);
    write_session(dir, "toggle", false);
    EXPECT_TRUE(acecode::list_archived_session_metadata(dir.string()).empty());

    write_session(dir, "toggle", true);
    EXPECT_EQ(sorted_ids(acecode::list_archived_session_metadata(dir.string())),
              (std::vector<std::string>{"toggle"}));
    fs::remove_all(root);
}

// 场景:登记过的 meta 文件还在,但这一刻读不出来(比如正被别的进程替换,或内容损坏)。
// 期望:这次列表里不出现,但索引条目保留;文件恢复正常后又能列出来。
// 非显然:只有「文件不存在」或「读出来已不是归档」才剔除,读失败不能当成已删除。
TEST(ArchivedSessionIndex, UnreadableMetaKeepsItsIndexEntry) {
    const auto root = make_data_root("unreadable");
    const auto dir = make_project(root, "aaaa000000000005");
    write_session(dir, "flaky", true);
    EXPECT_EQ(acecode::list_archived_session_metadata(dir.string()).size(), 1u);

    std::ofstream(dir / "flaky.meta.json", std::ios::binary | std::ios::trunc) << "{not json";
    EXPECT_TRUE(acecode::list_archived_session_metadata(dir.string()).empty());
    EXPECT_EQ(read_index(root)["sessions"].size(), 1u);

    write_session(dir, "flaky", true);
    EXPECT_EQ(acecode::list_archived_session_metadata(dir.string()).size(), 1u);
    fs::remove_all(root);
}

// 场景:索引文件被写坏。
// 期望:当作没有索引,重新扫描找回归档会话,并把索引重写成合法内容。
TEST(ArchivedSessionIndex, CorruptIndexIsRebuilt) {
    const auto root = make_data_root("corrupt");
    const auto dir = make_project(root, "aaaa000000000006");
    write_legacy_session(dir, "kept", true);
    EXPECT_EQ(acecode::list_archived_session_metadata(dir.string()).size(), 1u);

    std::ofstream(root / "archived-sessions.json", std::ios::binary | std::ios::trunc) << "{broken";
    EXPECT_EQ(sorted_ids(acecode::list_archived_session_metadata(dir.string())),
              (std::vector<std::string>{"kept"}));
    const auto index = read_index(root);
    ASSERT_TRUE(index.is_object());
    EXPECT_TRUE(index["scanned_projects"].contains("aaaa000000000006"));
    fs::remove_all(root);
}

// 场景:某个项目目录整个被删掉(删掉的工作区、跑完就删的测试目录),索引里还留着它的
//       归档条目和扫描标记。
// 期望:下一次写索引(这里是在别的目录归档一个会话)时一并清掉,索引不会越积越多。
// 回归:跑一遍 Web 单测就会在索引里留下一批测试目录的条目,之后永远不会再被检查到。
TEST(ArchivedSessionIndex, EntriesOfRemovedProjectDirectoriesAreDropped) {
    const auto root = make_data_root("removed_dir");
    const auto removed = make_project(root, "cccc000000000001");
    const auto kept = make_project(root, "cccc000000000002");
    write_session(removed, "doomed", true);
    EXPECT_EQ(acecode::list_archived_session_metadata(removed.string()).size(), 1u);
    fs::remove_all(removed);

    write_session(kept, "fresh", true);

    const auto index = read_index(root);
    ASSERT_TRUE(index.is_object());
    EXPECT_FALSE(index["scanned_projects"].contains("cccc000000000001"));
    ASSERT_EQ(index["sessions"].size(), 1u);
    EXPECT_EQ(index["sessions"][0]["id"], "fresh");
    fs::remove_all(root);
}

// 场景:项目目录不在 <根>/projects/ 下(测试或自定义调用方传进来的任意目录)。
// 期望:不建索引、不在目录外面留下文件,列表退回整目录扫描,结果照样正确。
TEST(ArchivedSessionIndex, NonStandardLayoutFallsBackToFullScan) {
    const auto root = make_data_root("plain");
    const auto dir = root / "plain-dir";
    fs::create_directories(dir);
    write_session(dir, "plain-archived", true);
    write_session(dir, "plain-live", false);

    EXPECT_TRUE(acecode::archived_session_index_path(dir.string()).empty());
    EXPECT_EQ(sorted_ids(acecode::list_archived_session_metadata(dir.string())),
              (std::vector<std::string>{"plain-archived"}));
    EXPECT_FALSE(fs::exists(root / "archived-sessions.json"));
    EXPECT_FALSE(fs::exists(dir / "archived-sessions.json"));
    fs::remove_all(root);
}

// 场景:无工作区会话每个一个目录;两个目录里有旧版本归档的会话,其中一个不是无工作区会话。
// 期望:第一次列出时逐个目录扫描,只返回无工作区的那个;之后不再调用目录清单,新归档的
//       无工作区会话经 write_meta 登记后直接出现。
// 回归:无工作区一路每次都要把全部无工作区目录整读一遍(实测 743 个文件冷读 3.5 秒)。
TEST(ArchivedSessionIndex, NoWorkspaceListingUsesIndexAfterFirstScan) {
    const auto root = make_data_root("no_workspace");
    const auto nw1 = make_project(root, "bbbb000000000001");
    const auto nw2 = make_project(root, "bbbb000000000002");
    const auto nw3 = make_project(root, "bbbb000000000003");
    write_legacy_session(nw1, "nw-archived", true, true);
    write_legacy_session(nw1, "nw-live", false, true);
    write_legacy_session(nw2, "not-no-workspace", true, false);

    int listed = 0;
    const auto dirs = [&listed, nw1, nw2, nw3] {
        ++listed;
        return std::vector<std::string>{nw1.string(), nw2.string(), nw3.string()};
    };
    const auto projects = (root / "projects").string();

    EXPECT_EQ(sorted_ids(acecode::list_archived_no_workspace_session_metadata(projects, dirs)),
              (std::vector<std::string>{"nw-archived"}));
    EXPECT_EQ(listed, 1);
    EXPECT_TRUE(read_index(root)["no_workspace_scanned"].get<bool>());

    write_session(nw3, "nw-new", true, true);
    EXPECT_EQ(sorted_ids(acecode::list_archived_no_workspace_session_metadata(projects, dirs)),
              (std::vector<std::string>{"nw-archived", "nw-new"}));
    EXPECT_EQ(listed, 1);
    fs::remove_all(root);
}
