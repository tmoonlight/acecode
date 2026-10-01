// 覆盖 src/host/session_host/memory_command.{hpp,cpp}(openspec unify-memory-system 6.1):
// /memory 的唯一文本实现,TUI 与网页 / 桌面对话调用同一函数,同一输入输出相同文本
// (edit 除外)。逐个子命令核对:list / view / forget / flush / off / on / reload / edit。

#include <gtest/gtest.h>

#include "memory/memory_service.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session_host/memory_command.hpp"
#include "session_host/memory_runtime.hpp"
#include "session_host/memory_scheduler.hpp"
#include "test_support/memory/memory_test_home.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>

namespace {

using acecode_test::MemoryTestHome;

struct CommandHarness {
    explicit CommandHarness(MemoryTestHome& home, acecode::MemoryConfig config = {})
        : cwd(home.workspace_cwd("ws")),
          runtime(home.service(config), acecode::MemorySurface::Tui) {
        session.start_session(cwd, "stub", "stub-model", acecode::SessionStorage::generate_session_id());
    }

    std::string run(const std::string& args, bool web = false) {
        acecode::MemoryCommandContext ctx;
        ctx.runtime = &runtime;
        ctx.session = &session;
        ctx.web = web;
        return acecode::dispatch_memory_command(args, ctx).text;
    }

    acecode::MemoryRegistry& workspace() {
        return *runtime.service()->workspace(session.current_project_dir());
    }

    void write(acecode::MemoryRegistry& registry, const std::string& name, acecode::MemoryType type,
               const std::string& source = acecode::kMemorySourceManual) {
        acecode::MemoryWriteRequest request;
        request.name = name;
        request.type = type;
        request.description = "about " + name;
        request.body = "body of " + name + "\n";
        request.source = source;
        std::string err;
        ASSERT_TRUE(registry.upsert(request, err).has_value()) << err;
    }

    std::string cwd;
    acecode::MemoryRuntime runtime;
    acecode::SessionManager session;
};

} // namespace

// 场景:全局与工作区各有条目(其中一条由记忆摘要生成),执行 /memory 与 /memory list。
// 期望:按作用域分组列出,标注类型、距今天数与 [summary];裸 /memory 等同 list;
// TUI 与网页两端文本完全相同。
TEST(MemoryCommandTest, ListGroupsByScopeAndIsIdenticalOnBothSurfaces) {
    MemoryTestHome home("memory-cmd-list");
    CommandHarness h(home);
    h.write(h.runtime.service()->global(), "prefs", acecode::MemoryType::User);
    h.write(h.workspace(), "build_steps", acecode::MemoryType::Project, acecode::kMemorySourceSummary);

    const std::string tui = h.run("list");
    EXPECT_EQ(tui, h.run("list", /*web=*/true));
    EXPECT_EQ(h.run(""), tui);
    EXPECT_NE(tui.find("Global memory ("), std::string::npos) << tui;
    EXPECT_NE(tui.find("Workspace memory ("), std::string::npos) << tui;
    EXPECT_NE(tui.find("[user] prefs"), std::string::npos) << tui;
    EXPECT_NE(tui.find("[project] build_steps"), std::string::npos) << tui;
    EXPECT_NE(tui.find("(today)"), std::string::npos) << tui;
    EXPECT_NE(tui.find("[summary]"), std::string::npos) << tui;
    EXPECT_LT(tui.find("Global memory"), tui.find("Workspace memory"));
}

// 场景:list 的作用域 / 类型过滤与非法参数。期望:只列所选作用域 / 类型;非法值给出提示。
TEST(MemoryCommandTest, ListFilters) {
    MemoryTestHome home("memory-cmd-filter");
    CommandHarness h(home);
    h.write(h.runtime.service()->global(), "prefs", acecode::MemoryType::User);
    h.write(h.runtime.service()->global(), "style", acecode::MemoryType::Feedback);
    h.write(h.workspace(), "layout", acecode::MemoryType::Project);

    const std::string global_only = h.run("list --scope=global");
    EXPECT_NE(global_only.find("prefs"), std::string::npos);
    EXPECT_EQ(global_only.find("Workspace memory"), std::string::npos);
    const std::string feedback = h.run("list --type=feedback");
    EXPECT_NE(feedback.find("style"), std::string::npos);
    EXPECT_EQ(feedback.find("prefs"), std::string::npos);
    EXPECT_NE(h.run("list --type=notes").find("Invalid type filter"), std::string::npos);
    EXPECT_NE(h.run("list --scope=team").find("Invalid scope"), std::string::npos);
}

// 场景:全局与工作区都有 build;/memory view build 与 view build --scope=global。
// 期望:先工作区后全局;显示作用域、类型、描述与正文;不存在时给出提示。
TEST(MemoryCommandTest, ViewPrefersWorkspace) {
    MemoryTestHome home("memory-cmd-view");
    CommandHarness h(home);
    h.write(h.runtime.service()->global(), "build", acecode::MemoryType::Feedback);
    h.write(h.workspace(), "build", acecode::MemoryType::Project);
    const std::string ws = h.run("view build");
    EXPECT_EQ(ws.rfind("[workspace] [project] build", 0), 0u) << ws;
    EXPECT_NE(ws.find("body of build"), std::string::npos);
    EXPECT_EQ(h.run("view build --scope=global").rfind("[global] [feedback] build", 0), 0u);
    EXPECT_NE(h.run("view ghost").find("No memory entry named 'ghost'"), std::string::npos);
    EXPECT_NE(h.run("view").find("Usage: /memory view"), std::string::npos);
}

// 场景:/memory forget build_steps。期望:条目文件与索引行被删除,并记录墓碑。
TEST(MemoryCommandTest, ForgetDeletesAndRecordsTombstone) {
    MemoryTestHome home("memory-cmd-forget");
    CommandHarness h(home);
    h.write(h.workspace(), "build_steps", acecode::MemoryType::Project);
    const std::string text = h.run("forget build_steps");
    EXPECT_NE(text.find("Forgot 'build_steps' from the workspace memory"), std::string::npos) << text;
    EXPECT_FALSE(std::filesystem::exists(h.workspace().dir() / "build_steps.md"));
    EXPECT_TRUE(h.runtime.service()->state().is_tombstoned(
        h.workspace().scope_key(), "build_steps", "", acecode::memory_now_ms()));
    EXPECT_EQ(h.run("list").find("build_steps"), std::string::npos);
}

// 场景:记忆摘要关闭时执行 /memory flush。
// 期望:只返回如何在个性化设置中开启的说明,不排队任何作业(没有调度器也不报错)。
TEST(MemoryCommandTest, FlushWithoutSummarizationExplainsHowToEnable) {
    MemoryTestHome home("memory-cmd-flush-off");
    CommandHarness h(home);
    const std::string text = h.run("flush");
    EXPECT_NE(text.find("Memory summarization is off"), std::string::npos) << text;
    EXPECT_NE(text.find("Settings > Personalization > Memory"), std::string::npos);
    EXPECT_EQ(h.runtime.scheduler(), nullptr);
}

// 场景:/memory off 之后 list,再 /memory on。
// 期望:off 关掉本会话记忆(SessionManager 状态翻转)并说明影响;list 末尾提示本会话已关闭;
// on 恢复。
TEST(MemoryCommandTest, OffAndOnToggleSessionMemory) {
    MemoryTestHome home("memory-cmd-toggle");
    CommandHarness h(home);
    EXPECT_NE(h.run("off").find("Memory is off for this session"), std::string::npos);
    EXPECT_FALSE(h.session.memory_enabled());
    EXPECT_NE(h.run("list").find("/memory on to turn it back on"), std::string::npos);
    EXPECT_EQ(h.run("on"), "Memory is on for this session.");
    EXPECT_TRUE(h.session.memory_enabled());
}

// 场景:另一个进程直接往磁盘写了条目,执行 /memory reload。期望:报告两个作用域的条目数。
TEST(MemoryCommandTest, ReloadCountsBothScopes) {
    MemoryTestHome home("memory-cmd-reload");
    CommandHarness h(home);
    auto other = home.service();
    std::string err;
    other->global().upsert("a", acecode::MemoryType::User, "a", "a\n", acecode::MemoryWriteMode::Upsert, err);
    other->workspace(h.session.current_project_dir())
        ->upsert("b", acecode::MemoryType::Project, "b", "b\n", acecode::MemoryWriteMode::Upsert, err);
    EXPECT_EQ(h.run("reload"), "Reloaded 1 global and 1 workspace memory entries.");
}

// 场景:/memory edit <name> 在 TUI 与网页。
// 期望:TUI 返回要用编辑器打开的文件路径;网页只提示去设置页编辑;找不到时提示。
TEST(MemoryCommandTest, EditDiffersBySurface) {
    MemoryTestHome home("memory-cmd-edit");
    CommandHarness h(home);
    h.write(h.runtime.service()->global(), "prefs", acecode::MemoryType::User);
    acecode::MemoryCommandContext ctx;
    ctx.runtime = &h.runtime;
    ctx.session = &h.session;
    const auto tui = acecode::dispatch_memory_command("edit prefs", ctx);
    EXPECT_NE(tui.edit_path.find("prefs.md"), std::string::npos);
    ctx.web = true;
    const auto web = acecode::dispatch_memory_command("edit prefs", ctx);
    EXPECT_TRUE(web.edit_path.empty());
    EXPECT_NE(web.text.find("Settings > Personalization > Memory"), std::string::npos);
    EXPECT_NE(h.run("edit ghost").find("No memory entry named 'ghost'"), std::string::npos);
}

// 场景:设置里关闭「使用记忆」后执行 /memory list;没有记忆运行时时执行任意子命令。
// 期望:分别提示记忆已关闭 / 记忆不可用;未知子命令给出用法。
TEST(MemoryCommandTest, DisabledOrUnavailableMemory) {
    MemoryTestHome home("memory-cmd-disabled");
    acecode::MemoryConfig off;
    off.enabled = false;
    CommandHarness h(home, off);
    EXPECT_NE(h.run("list").find("turned off in Settings"), std::string::npos);
    EXPECT_NE(h.run("bogus").find("turned off in Settings"), std::string::npos);

    acecode::MemoryCommandContext none;
    EXPECT_EQ(acecode::dispatch_memory_command("list", none).text,
              "Memory is not available in this session.");

    CommandHarness on(home);
    EXPECT_NE(on.run("bogus").find("Usage:"), std::string::npos);
}

// 场景:记忆摘要开启、调度器在运行时执行 /memory flush。
// 期望:命令立即返回「已开始整理」,不阻塞;调度线程随即处理并经宿主通知回报结果。
TEST(MemoryCommandTest, FlushWithSummarizationRunsAsynchronously) {
    MemoryTestHome home("memory-cmd-flush-on");
    acecode::MemoryConfig on;
    on.summary.enabled = true;
    CommandHarness h(home, on);
    acecode::ChatMessage user;
    user.role = "user";
    user.content = "remember: deploy only from main";
    h.session.on_message(user);

    std::mutex mu;
    std::condition_variable cv;
    std::string notified;
    acecode::MemorySchedulerHost host;
    host.interval = std::chrono::hours(1);  // 只验证 flush 路径,不让周期调度插进来
    host.complete = [](const std::string&, const std::string&, const std::string&, std::string&,
                       bool&) { return std::string(R"({"outcome":"noop","observations":[]})"); };
    host.notify = [&](const std::string& session_id, const std::string& text) {
        std::lock_guard<std::mutex> lock(mu);
        notified = session_id + "|" + text;
        cv.notify_all();
    };
    h.runtime.start_summary_scheduler(std::move(host));
    ASSERT_NE(h.runtime.scheduler(), nullptr);

    const std::string reply = h.run("flush");
    EXPECT_NE(reply.find("Started organizing memory"), std::string::npos) << reply;
    {
        std::unique_lock<std::mutex> lock(mu);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(10), [&] { return !notified.empty(); }));
    }
    EXPECT_EQ(notified.rfind(h.session.current_session_id() + "|Memory flush finished", 0), 0u) << notified;
    h.runtime.stop_summary_scheduler();
}
