// 覆盖 openspec unify-memory-system 第 2 组的存储层行为:
// - MemoryRegistry 按目录实例化:全局与工作区两个作用域互不影响(2.1)
// - frontmatter 来源字段:系统写入补齐 created_at / updated_at / source /
//   source_sessions,旧条目 created_at 取文件原修改时间,未知字段原样保留(2.2)
// - 跨进程写锁:两个服务实例(各自一个状态库连接)交替 / 并发写同一作用域,
//   MEMORY.md 保留全部条目(2.5)
// - 作用域外的写入(经符号链接逃逸)一律拒绝(4.3)
// - 删除条目记墓碑、重置只清一个作用域

#include <gtest/gtest.h>

#include "memory/memory_frontmatter.hpp"
#include "memory/memory_paths.hpp"
#include "memory/memory_registry.hpp"
#include "memory/memory_service.hpp"
#include "memory/memory_state_store.hpp"
#include "test_support/memory/memory_test_home.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace {

using acecode_test::MemoryTestHome;

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

void write_raw(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary);
    ofs << content;
}

bool upsert(acecode::MemoryRegistry& registry, const std::string& name,
            acecode::MemoryType type = acecode::MemoryType::User) {
    std::string err;
    const bool ok = registry.upsert(name, type, "desc " + name, "body " + name + "\n",
                                    acecode::MemoryWriteMode::Upsert, err).has_value();
    EXPECT_TRUE(ok) << err;
    return ok;
}

} // namespace

// 场景:全局与工作区各写一条同名条目,再删掉工作区那条。
// 期望:两个作用域各自的文件与 MEMORY.md 互不影响;删除只作用于工作区。
TEST(MemoryScopeStoreTest, GlobalAndWorkspaceScopesAreIndependent) {
    MemoryTestHome home("memory-scope-independent");
    auto memory = home.service();
    const std::string project_dir = MemoryTestHome::project_dir(home.workspace_cwd("ws"));
    auto workspace = memory->workspace(project_dir);
    ASSERT_TRUE(workspace);
    EXPECT_EQ(workspace->dir(), acecode::workspace_memory_dir(project_dir));

    ASSERT_TRUE(upsert(memory->global(), "shared"));
    ASSERT_TRUE(upsert(*workspace, "shared", acecode::MemoryType::Project));
    ASSERT_TRUE(upsert(*workspace, "only_ws", acecode::MemoryType::Project));

    EXPECT_EQ(memory->global().size(), 1u);
    EXPECT_EQ(workspace->size(), 2u);
    EXPECT_EQ(read_file(memory->global().dir() / "MEMORY.md").find("only_ws"), std::string::npos);

    std::string err;
    ASSERT_TRUE(workspace->remove("shared", err)) << err;
    memory->global().reload();
    EXPECT_TRUE(memory->global().find("shared").has_value());
    EXPECT_FALSE(workspace->find("shared").has_value());
    // 同一项目目录复用同一个存储实例。
    EXPECT_EQ(memory->workspace(project_dir).get(), workspace.get());
    EXPECT_EQ(memory->workspace("").get(), nullptr);
}

// 场景:一个没有来源字段、带用户自定义字段 owner 的旧条目被 memory_write 以 update 改写。
// 期望:写回后有 updated_at(本次)与 source: manual,created_at 取文件原修改时间,
// owner 字段原样保留;再次读出时正文没有多出空行。
TEST(MemoryScopeStoreTest, LegacyEntryGetsProvenanceAndKeepsUnknownFields) {
    MemoryTestHome home("memory-scope-provenance");
    const fs::path path = acecode::get_memory_dir() / "legacy.md";
    write_raw(path,
              "---\nname: \"legacy\"\ndescription: \"old desc\"\ntype: user\n"
              "owner: team-a\ntags:\n  - one\n  - two\n---\n\nold body\n");
    const auto old_time = fs::last_write_time(path) - std::chrono::hours(72);
    fs::last_write_time(path, old_time);
    const std::string expected_created = acecode::memory_file_mtime_iso8601(path);

    acecode::MemoryRegistry registry;
    acecode::MemoryWriteRequest request;
    request.name = "legacy";
    request.type = acecode::MemoryType::User;
    request.description = "new desc";
    request.body = "new body\n";
    request.mode = acecode::MemoryWriteMode::Update;
    request.source_sessions = {"s-1"};
    std::string err;
    ASSERT_TRUE(registry.upsert(request, err).has_value()) << err;

    const std::string content = read_file(path);
    EXPECT_NE(content.find("owner: team-a"), std::string::npos) << content;
    EXPECT_NE(content.find("  - two"), std::string::npos) << content;
    auto entry = acecode::parse_memory_entry_file(path);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->created_at, expected_created);
    EXPECT_FALSE(entry->updated_at.empty());
    EXPECT_NE(entry->updated_at, entry->created_at);
    EXPECT_EQ(entry->source, "manual");
    ASSERT_EQ(entry->source_sessions.size(), 1u);
    EXPECT_EQ(entry->body, "new body\n");
    EXPECT_NE(entry->extra_frontmatter.find("owner: team-a"), std::string::npos);
}

// 场景:描述里有引号与反斜杠的条目被系统反复改写(例如遗忘只改来源字段)。
// 期望:render → parse 往返稳定,不会每写一次多一层转义。
TEST(MemoryScopeStoreTest, QuotedDescriptionRoundTripsWithoutGrowingEscapes) {
    MemoryTestHome home("memory-scope-escape");
    acecode::MemoryRegistry registry;
    std::string err;
    const std::string description = "say \"hi\" to C:\\tools";
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(registry.upsert("quoted", acecode::MemoryType::User, description, "b\n",
                                    acecode::MemoryWriteMode::Upsert, err).has_value()) << err;
        registry.reload();
        auto entry = registry.find("quoted");
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->description, description);
    }
}

// 场景:两个进程(两个服务实例、两个状态库连接)启动时都扫过空目录,之后交替写入。
// 期望:MEMORY.md 保留双方写入的全部条目。扩展自修复分支的
// UpsertKeepsEntriesWrittenByAnotherProcess,这次两边都走状态库写锁。
TEST(MemoryScopeStoreTest, AlternatingWritesFromTwoProcessesKeepEveryIndexLine) {
    MemoryTestHome home("memory-scope-two-process");
    auto a = home.service();
    auto b = home.service();
    a->global().scan();
    b->global().scan();
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(upsert(a->global(), "a_" + std::to_string(i)));
        ASSERT_TRUE(upsert(b->global(), "b_" + std::to_string(i)));
    }
    const std::string index = read_file(acecode::get_memory_index_path());
    for (int i = 0; i < 5; ++i) {
        EXPECT_NE(index.find("a_" + std::to_string(i) + ".md"), std::string::npos) << index;
        EXPECT_NE(index.find("b_" + std::to_string(i) + ".md"), std::string::npos) << index;
    }
}

// 场景:两个进程同时(两个线程各用自己的服务实例)向同一作用域写 10 条。
// 期望:跨进程写锁把「重扫 + 写文件 + 重建索引」串行化,最终 20 条全在索引里。
TEST(MemoryScopeStoreTest, ConcurrentWritesFromTwoProcessesKeepEveryIndexLine) {
    MemoryTestHome home("memory-scope-concurrent");
    auto a = home.service();
    auto b = home.service();
    auto worker = [](std::shared_ptr<acecode::MemoryService> memory, const std::string& prefix) {
        for (int i = 0; i < 10; ++i) {
            std::string err;
            memory->global().upsert(prefix + std::to_string(i), acecode::MemoryType::User,
                                    "d", "b\n", acecode::MemoryWriteMode::Upsert, err);
        }
    };
    std::thread t1(worker, a, "x");
    std::thread t2(worker, b, "y");
    t1.join();
    t2.join();
    const std::string index = read_file(acecode::get_memory_index_path());
    for (int i = 0; i < 10; ++i) {
        EXPECT_NE(index.find("x" + std::to_string(i) + ".md"), std::string::npos) << index;
        EXPECT_NE(index.find("y" + std::to_string(i) + ".md"), std::string::npos) << index;
    }
}

// 场景:作用域目录里有一个指向目录外文件的符号链接 evil.md,模型写入 evil。
// 期望:目标解析到作用域外,写入被拒绝,外部文件不变。系统不允许建符号链接时跳过。
TEST(MemoryScopeStoreTest, SymlinkEscapeIsRejected) {
    MemoryTestHome home("memory-scope-symlink");
    const fs::path outside = home.root() / "outside.md";
    write_raw(outside, "outside\n");
    std::error_code ec;
    fs::create_symlink(outside, acecode::get_memory_dir() / "evil.md", ec);
    if (ec) GTEST_SKIP() << "symlink creation not permitted: " << ec.message();

    acecode::MemoryRegistry registry;
    std::string err;
    EXPECT_FALSE(registry.upsert("evil", acecode::MemoryType::User, "d", "pwned\n",
                                 acecode::MemoryWriteMode::Upsert, err).has_value());
    EXPECT_NE(err.find("escapes"), std::string::npos) << err;
    EXPECT_EQ(read_file(outside), "outside\n");
}

// 场景:用户删除一条记忆(/memory forget 或设置页删除)。
// 期望:文件与索引行消失,并记录该作用域、名字与描述(标题)的墓碑。
TEST(MemoryScopeStoreTest, DeleteEntryRecordsTombstone) {
    MemoryTestHome home("memory-scope-tombstone");
    auto memory = home.service();
    ASSERT_TRUE(upsert(memory->global(), "old_rule"));
    std::string err;
    ASSERT_TRUE(memory->delete_entry(acecode::MemoryScope::Global, "", "old_rule", err)) << err;
    EXPECT_FALSE(fs::exists(acecode::get_memory_dir() / "old_rule.md"));
    EXPECT_EQ(read_file(acecode::get_memory_index_path()).find("old_rule"), std::string::npos);
    const auto now = acecode::memory_now_ms();
    EXPECT_TRUE(memory->state().is_tombstoned("global", "old_rule", "", now));
    EXPECT_TRUE(memory->state().is_tombstoned("global", "renamed", "DESC   OLD_RULE", now));
}

// 场景:重置工作区作用域(设置页二次确认后)。
// 期望:该作用域的条目、索引、收件箱与归档全部清空,全局作用域不受影响。
TEST(MemoryScopeStoreTest, ResetClearsOnlyOneScope) {
    MemoryTestHome home("memory-scope-reset");
    auto memory = home.service();
    const std::string project_dir = MemoryTestHome::project_dir(home.workspace_cwd("ws"));
    auto workspace = memory->workspace(project_dir);
    ASSERT_TRUE(upsert(memory->global(), "keep"));
    ASSERT_TRUE(upsert(*workspace, "drop", acecode::MemoryType::Project));
    write_raw(acecode::memory_inbox_dir(workspace->dir()) / "s-0-3.json", "{}");
    write_raw(acecode::memory_archive_dir(workspace->dir()) / "2026-09-01" / "s-0-1.json", "{}");

    std::string err;
    ASSERT_TRUE(memory->reset_scope(acecode::MemoryScope::Workspace, project_dir, err)) << err;
    EXPECT_EQ(workspace->size(), 0u);
    EXPECT_FALSE(fs::exists(workspace->dir() / "drop.md"));
    EXPECT_FALSE(fs::exists(workspace->dir() / "MEMORY.md"));
    EXPECT_FALSE(fs::exists(acecode::memory_inbox_dir(workspace->dir())));
    EXPECT_FALSE(fs::exists(acecode::memory_archive_dir(workspace->dir())));
    memory->global().reload();
    EXPECT_TRUE(memory->global().find("keep").has_value());
}

// 场景:作用域目录下有 skills/ 等非保留子目录(里面也有 .md)。
// 期望:只有顶层 *.md 算条目,子目录不被扫描。
TEST(MemoryScopeStoreTest, NestedDirectoriesAreIgnored) {
    MemoryTestHome home("memory-scope-nested");
    write_raw(acecode::get_memory_dir() / "skills" / "inner.md",
              "---\nname: \"inner\"\ndescription: \"d\"\ntype: user\n---\n\nbody\n");
    acecode::MemoryRegistry registry;
    registry.scan();
    EXPECT_EQ(registry.size(), 0u);
}
