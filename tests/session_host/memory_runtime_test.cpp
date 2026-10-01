// 覆盖 src/host/session_host/memory_runtime.{hpp,cpp}(openspec unify-memory-system 3.1 / 3.3 / 9.4):
// - create_memory_runtime 建好全局目录;headless 不启动记忆摘要调度器
// - 工具按「会话项目目录」解析工作区作用域,不看进程 cwd:daemon 一个进程服务多个工作区
// - 子会话与父会话的项目目录相同,读到的是父会话工作区的条目
// - 会话经 SessionStorage::purge_session_files 永久删除后,运行时撤回它的摘要条目

#include <gtest/gtest.h>

#include "memory/memory_paths.hpp"
#include "memory/memory_service.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session_host/memory_runtime.hpp"
#include "session_host/memory_scheduler.hpp"
#include "test_support/memory/memory_test_home.hpp"
#include "tool/tool_executor.hpp"
#include "utils/paths.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

using acecode_test::MemoryTestHome;

nlohmann::json call(acecode::ToolExecutor& tools, const std::string& name, const std::string& args,
                    acecode::SessionManager& session) {
    acecode::ToolContext ctx;
    ctx.session_manager = &session;
    const auto result = tools.execute(name, args, ctx);
    EXPECT_TRUE(result.success) << result.output;
    return nlohmann::json::parse(result.output, nullptr, false);
}

} // namespace

// 场景:三个入口各自调用 create_memory_runtime。
// 期望:全局目录被创建并扫描;headless 运行时即使被要求也不启动调度器;
// 两个记忆工具都注册到工具表(headless 的 --list-tools 能看到它们)。
TEST(MemoryRuntimeTest, CreatesGlobalDirAndRegistersTools) {
    MemoryTestHome home("memory-runtime-create");
    std::error_code ec;
    fs::remove_all(acecode::get_memory_dir(), ec);
    auto runtime = acecode::create_memory_runtime(acecode::AppConfig{}, acecode::get_acecode_dir(),
                                                  acecode::MemorySurface::Headless);
    EXPECT_TRUE(fs::is_directory(acecode::get_memory_dir()));
    EXPECT_TRUE(runtime->service()->enabled());

    acecode::ToolExecutor tools;
    runtime->register_tools(tools);
    EXPECT_TRUE(tools.has_tool("memory_read"));
    EXPECT_TRUE(tools.has_tool("memory_write"));

    runtime->start_summary_scheduler(acecode::MemorySchedulerHost{});
    EXPECT_EQ(runtime->scheduler(), nullptr);
}

// 场景:一个 daemon 进程(进程 cwd 不属于任何会话工作区)同时服务工作区 A 与 B 的会话,
// 两边都写 project 类型记忆、再列出。
// 期望:A 的会话只看到 A 的条目,B 的只看到 B 的;条目落在各自
// <data_dir>/projects/<hash>/memory/ 下。
TEST(MemoryRuntimeTest, ResolvesWorkspaceFromEachSessionNotProcessCwd) {
    MemoryTestHome home("memory-runtime-multi-workspace");
    auto runtime = acecode::create_memory_runtime(acecode::AppConfig{}, acecode::get_acecode_dir(),
                                                  acecode::MemorySurface::Daemon);
    acecode::ToolExecutor tools;
    runtime->register_tools(tools);

    acecode::SessionManager a;
    acecode::SessionManager b;
    a.start_session(home.workspace_cwd("a"), "stub", "m", acecode::SessionStorage::generate_session_id());
    b.start_session(home.workspace_cwd("b"), "stub", "m", acecode::SessionStorage::generate_session_id());

    call(tools, "memory_write", R"({"name":"rule_a","type":"project","description":"a","body":"a"})", a);
    call(tools, "memory_write", R"({"name":"rule_b","type":"project","description":"b","body":"b"})", b);

    const auto list_a = call(tools, "memory_read", "{}", a);
    ASSERT_EQ(list_a["count"].get<int>(), 1);
    EXPECT_EQ(list_a["entries"][0]["name"], "rule_a");
    const auto list_b = call(tools, "memory_read", "{}", b);
    ASSERT_EQ(list_b["count"].get<int>(), 1);
    EXPECT_EQ(list_b["entries"][0]["name"], "rule_b");
    EXPECT_TRUE(fs::exists(acecode::workspace_memory_dir(a.current_project_dir()) / "rule_a.md"));
    EXPECT_TRUE(fs::exists(acecode::workspace_memory_dir(b.current_project_dir()) / "rule_b.md"));
}

// 场景:工作区 W 的会话派生子会话(与父会话同一 cwd,只是带父会话 id),子会话调用
// memory_read({})。
// 期望:返回全局与 W 的工作区条目 —— 子会话沿用父会话的工作区作用域。
TEST(MemoryRuntimeTest, SubagentSharesParentWorkspaceScope) {
    MemoryTestHome home("memory-runtime-subagent");
    auto runtime = acecode::create_memory_runtime(acecode::AppConfig{}, acecode::get_acecode_dir(),
                                                  acecode::MemorySurface::Daemon);
    acecode::ToolExecutor tools;
    runtime->register_tools(tools);
    const std::string cwd = home.workspace_cwd("w");

    acecode::SessionManager parent;
    parent.start_session(cwd, "stub", "m", acecode::SessionStorage::generate_session_id());
    call(tools, "memory_write", R"({"name":"shared_rule","type":"project","description":"d","body":"b"})", parent);
    call(tools, "memory_write", R"({"name":"pref","type":"user","description":"d","body":"b"})", parent);

    acecode::SessionManager child;
    child.start_session(cwd, "stub", "m", acecode::SessionStorage::generate_session_id());
    child.set_parent_session_id(parent.current_session_id());
    const auto listed = call(tools, "memory_read", "{}", child);
    ASSERT_EQ(listed["count"].get<int>(), 2);
    EXPECT_EQ(child.current_project_dir(), parent.current_project_dir());
}

// 场景:摘要条目 only_s 只来源于会话 S;S 的会话文件被永久删除(任意清除入口最终都走
// purge_session_files)。
// 期望:运行时监听到删除,撤回 only_s 并记墓碑;运行时析构后不再监听。
TEST(MemoryRuntimeTest, PurgingSessionRetractsItsSummaryEntries) {
    MemoryTestHome home("memory-runtime-purge");
    auto runtime = acecode::create_memory_runtime(acecode::AppConfig{}, acecode::get_acecode_dir(),
                                                  acecode::MemorySurface::Daemon);
    acecode::SessionManager session;
    session.start_session(home.workspace_cwd("w"), "stub", "m", acecode::SessionStorage::generate_session_id());
    acecode::ChatMessage user;
    user.role = "user";
    user.content = "hello";
    session.on_message(user);
    const std::string id = session.current_session_id();
    const std::string project_dir = session.current_project_dir();
    session.finalize();

    auto workspace = runtime->service()->workspace(project_dir);
    acecode::MemoryWriteRequest request;
    request.name = "only_s";
    request.type = acecode::MemoryType::Project;
    request.description = "from S";
    request.body = "b\n";
    request.source = acecode::kMemorySourceSummary;
    request.source_sessions = {id};
    std::string err;
    ASSERT_TRUE(workspace->upsert(request, err).has_value()) << err;

    ASSERT_TRUE(acecode::SessionStorage::purge_session_files(project_dir, id, &err)) << err;
    workspace->reload();
    EXPECT_FALSE(workspace->find("only_s").has_value());
    EXPECT_TRUE(runtime->service()->state().is_tombstoned(workspace->scope_key(), "only_s", "",
                                                          acecode::memory_now_ms()));
}
