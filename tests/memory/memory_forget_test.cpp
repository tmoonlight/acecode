// 覆盖收件箱 / 归档存储(src/domain/memory/memory_inbox.*)与会话遗忘
// (src/domain/memory/memory_forget.*,openspec unify-memory-system 9.4 / D14):
// - 观察文件按 <会话>-<from>-<to>.json 命名,同一范围重放覆盖同一文件
// - 归档按日期目录移动,30 天后清理
// - 会话被永久删除:删观察、摘要条目移除该会话来源、无来源者删除并记墓碑、
//   手写条目不动、提炼进度清掉;删除走 SessionStorage::purge_session_files 的监听

#include <gtest/gtest.h>

#include "memory/memory_forget.hpp"
#include "memory/memory_frontmatter.hpp"
#include "memory/memory_inbox.hpp"
#include "memory/memory_paths.hpp"
#include "memory/memory_service.hpp"
#include "session/session_purge_listeners.hpp"
#include "test_support/memory/memory_test_home.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

using acecode_test::MemoryTestHome;

acecode::MemoryObservationFile observation_file(const std::string& session, std::int64_t from,
                                                std::int64_t to, int count) {
    acecode::MemoryObservationFile file;
    file.session_id = session;
    file.from = from;
    file.to = to;
    for (int i = 0; i < count; ++i) {
        file.observations.push_back({"", acecode::MemoryType::Project,
                                     "title " + std::to_string(i), "statement " + std::to_string(i)});
    }
    return file;
}

void write_summary_entry(acecode::MemoryRegistry& registry, const std::string& name,
                         std::vector<std::string> sessions) {
    acecode::MemoryWriteRequest request;
    request.name = name;
    request.type = acecode::MemoryType::Project;
    request.description = "summary " + name;
    request.body = "body\n";
    request.source = acecode::kMemorySourceSummary;
    request.source_sessions = std::move(sessions);
    request.replace_source_sessions = true;
    std::string err;
    ASSERT_TRUE(registry.upsert(request, err).has_value()) << err;
}

} // namespace

// 场景:提炼写观察文件,崩溃后同一范围重放(观察内容变了)。
// 期望:文件名稳定、内容被覆盖不重复;id 按文件名 + 序号编号;计数正确。
TEST(MemoryInboxTest, ObservationFilesAreIdempotentPerRange) {
    MemoryTestHome home("memory-inbox-idempotent");
    const fs::path scope = acecode::get_memory_dir();
    auto first = observation_file("s1", 0, 8, 3);
    ASSERT_TRUE(acecode::write_memory_observation_file(scope, first));
    auto replay = observation_file("s1", 0, 8, 2);
    ASSERT_TRUE(acecode::write_memory_observation_file(scope, replay));

    const auto inbox = acecode::list_memory_inbox(scope);
    ASSERT_EQ(inbox.size(), 1u);
    EXPECT_EQ(inbox[0].observations.size(), 2u);
    EXPECT_EQ(inbox[0].observations[1].id, "s1-0-8#1");
    EXPECT_EQ(acecode::count_memory_inbox_observations(scope), 2u);
}

// 场景:整合成功后把本批观察归档;31 天后清理归档。
// 期望:文件从 inbox 移到 archive/<日期>/;重放归档时已移走的文件跳过;
// 过期日期目录被删除,近期的保留。
TEST(MemoryInboxTest, ArchiveMovesFilesAndCleansUpOldDays) {
    MemoryTestHome home("memory-inbox-archive");
    const fs::path scope = acecode::get_memory_dir();
    auto file = observation_file("s2", 0, 4, 1);
    ASSERT_TRUE(acecode::write_memory_observation_file(scope, file));
    ASSERT_TRUE(acecode::archive_memory_observation_files(scope, {file.path}, "2026-08-01"));
    EXPECT_FALSE(fs::exists(file.path));
    EXPECT_TRUE(fs::exists(acecode::memory_archive_dir(scope) / "2026-08-01" / file.path.filename()));
    ASSERT_TRUE(acecode::archive_memory_observation_files(scope, {file.path}, "2026-08-01"));
    fs::create_directories(acecode::memory_archive_dir(scope) / "2026-09-25");

    const auto now = acecode::parse_memory_iso8601("2026-10-01T00:00:00Z");
    ASSERT_TRUE(now.has_value());
    EXPECT_EQ(acecode::cleanup_memory_archive(scope, *now, 30), 1);
    EXPECT_FALSE(fs::exists(acecode::memory_archive_dir(scope) / "2026-08-01"));
    EXPECT_TRUE(fs::exists(acecode::memory_archive_dir(scope) / "2026-09-25"));
}

// 场景:条目 only_s 只来源于会话 S;shared 来源于 S 与 T;手写条目 manual_note
// 的来源也记着 S;S 在收件箱与归档里都有观察。永久删除 S。
// 期望:only_s 删除并记墓碑;shared 只移除 S;手写条目保留;S 的观察全部删除,
// T 的保留;S 的提炼进度清掉。
TEST(MemoryForgetTest, ForgetSessionRetractsItsContribution) {
    MemoryTestHome home("memory-forget-session");
    auto memory = home.service();
    const std::string project_dir = MemoryTestHome::project_dir(home.workspace_cwd("ws"));
    auto workspace = memory->workspace(project_dir);
    write_summary_entry(*workspace, "only_s", {"S"});
    write_summary_entry(*workspace, "shared", {"S", "T"});
    acecode::MemoryWriteRequest manual;
    manual.name = "manual_note";
    manual.type = acecode::MemoryType::User;
    manual.description = "written by hand";
    manual.body = "keep me\n";
    manual.source_sessions = {"S"};
    std::string err;
    ASSERT_TRUE(memory->global().upsert(manual, err).has_value()) << err;

    auto s_inbox = observation_file("S", 0, 5, 2);
    auto t_inbox = observation_file("T", 0, 5, 1);
    auto s_archived = observation_file("S", 5, 9, 1);
    ASSERT_TRUE(acecode::write_memory_observation_file(workspace->dir(), s_inbox));
    ASSERT_TRUE(acecode::write_memory_observation_file(workspace->dir(), t_inbox));
    ASSERT_TRUE(acecode::write_memory_observation_file(workspace->dir(), s_archived));
    ASSERT_TRUE(acecode::archive_memory_observation_files(workspace->dir(), {s_archived.path}, "2026-09-30"));
    memory->state().commit_extraction("S", project_dir, 9, "9@x", acecode::memory_now_ms());

    const auto result = acecode::forget_memory_session(*memory, "S", project_dir);
    EXPECT_EQ(result.observations_removed, 3);
    EXPECT_EQ(result.entries_deleted, 1);
    EXPECT_EQ(result.entries_updated, 1);

    workspace->reload();
    EXPECT_FALSE(workspace->find("only_s").has_value());
    auto shared = workspace->find("shared");
    ASSERT_TRUE(shared.has_value());
    EXPECT_EQ(shared->source_sessions, std::vector<std::string>{"T"});
    memory->global().reload();
    EXPECT_TRUE(memory->global().find("manual_note").has_value());
    EXPECT_EQ(acecode::count_memory_inbox_observations(workspace->dir()), 1u);
    EXPECT_TRUE(memory->state().is_tombstoned(workspace->scope_key(), "only_s", "",
                                              acecode::memory_now_ms()));
    EXPECT_FALSE(memory->state().extraction("S").has_value());
}

// 场景:会话清除走 SessionStorage::purge_session_files,所有入口最终都落到这里。
// 期望:purge 成功后通知监听者(记忆运行时据此遗忘);移除监听后不再收到。
TEST(MemoryForgetTest, PurgeNotifiesListeners) {
    MemoryTestHome home("memory-forget-purge");
    const std::string project_dir = MemoryTestHome::project_dir(home.workspace_cwd("ws"));
    fs::create_directories(acecode::path_from_utf8(project_dir));
    const std::string id = "20261001-120000-ab12";
    {
        std::ofstream(acecode::path_from_utf8(acecode::SessionStorage::session_path(project_dir, id)))
            << "{}\n";
        std::ofstream(acecode::path_from_utf8(acecode::SessionStorage::meta_path(project_dir, id)))
            << "{}\n";
    }
    std::vector<std::string> seen;
    const auto listener = acecode::add_session_purge_listener(
        [&seen](const std::string& dir, const std::string& session) { seen.push_back(dir + "|" + session); });
    std::string error;
    ASSERT_TRUE(acecode::SessionStorage::purge_session_files(project_dir, id, &error)) << error;
    acecode::remove_session_purge_listener(listener);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], project_dir + "|" + id);
}
