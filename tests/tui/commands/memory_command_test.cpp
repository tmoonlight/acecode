// 覆盖 src/apps/tui/commands/memory_command.{hpp,cpp}(openspec unify-memory-system 6.2):
// TUI 的 /memory 改为调用共享的 dispatch_memory_command,对话里出现的系统消息与
// 共享实现(也是网页 /memory 的文本)逐字相同;/memory off 作用于当前会话。

#include "test_support/agent/agent_loop_fixture.hpp"
#include "test_support/memory/memory_test_home.hpp"
#include <gtest/gtest.h>

#include "agent/agent_loop.hpp"
#include "config/config.hpp"
#include "permissions/permissions.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/token_tracker.hpp"
#include "session_host/memory_command.hpp"
#include "session_host/memory_runtime.hpp"
#include "tool/tool_executor.hpp"
#include "tui/commands/command_registry.hpp"
#include "tui/commands/memory_command.hpp"

#include <memory>

namespace {

using acecode_test::MemoryTestHome;

class MemoryCommandHarness {
public:
    explicit MemoryCommandHarness(MemoryTestHome& home)
        : cwd_(home.workspace_cwd("tui")),
          runtime_(home.service(), acecode::MemorySurface::Tui),
          loop_(std::make_unique<acecode::AgentLoop>(
              acecode_test::AgentLoopFixture::dependencies(
                  [] { return std::shared_ptr<acecode::LlmProvider>{}; }, tools_,
                  acecode::AgentCallbacks{}, perms_, &sm_),
              acecode_test::AgentLoopFixture::configuration(cwd_))) {
        loop_->start();
        sm_.start_session(cwd_, "stub", "model", acecode::SessionStorage::generate_session_id());
        acecode::register_memory_command(registry_);
    }

    ~MemoryCommandHarness() { loop_->shutdown(); }

    // 执行一条斜杠命令,返回它追加到对话里的最后一条系统消息。
    std::string dispatch(const std::string& text) {
        acecode::CommandContext ctx{state_, *loop_, nullptr, config_, tracker_, perms_};
        ctx.session_manager = &sm_;
        ctx.memory = &runtime_;
        ctx.command_registry = &registry_;
        ctx.cwd = cwd_;
        EXPECT_TRUE(registry_.dispatch(text, ctx));
        std::lock_guard<std::mutex> lock(state_.mu);
        return state_.conversation.empty() ? std::string{} : state_.conversation.back().content;
    }

    std::string cwd_;
    acecode::MemoryRuntime runtime_;
    acecode::TuiState state_;
    acecode::CommandRegistry registry_;
    acecode::SessionManager sm_;
    acecode::ToolExecutor tools_;
    acecode::PermissionManager perms_;
    acecode::AppConfig config_;
    acecode::TokenTracker tracker_;
    std::unique_ptr<acecode::AgentLoop> loop_;
};

} // namespace

// 场景:TUI 里执行 /memory list,同一会话再用共享实现生成网页那份文本。
// 期望:对话里出现的系统消息与 dispatch_memory_command 输出逐字相同。
TEST(TuiMemoryCommand, ListUsesSharedDispatcherText) {
    MemoryTestHome home("tui-memory-list");
    MemoryCommandHarness h(home);
    std::string err;
    ASSERT_TRUE(h.runtime_.service()->global().upsert("prefs", acecode::MemoryType::User,
                                                      "prefers pnpm", "use pnpm\n",
                                                      acecode::MemoryWriteMode::Upsert, err)) << err;
    const std::string shown = h.dispatch("/memory list");
    acecode::MemoryCommandContext ctx;
    ctx.runtime = &h.runtime_;
    ctx.session = &h.sm_;
    ctx.web = true;
    EXPECT_EQ(shown, acecode::dispatch_memory_command("list", ctx).text);
    EXPECT_NE(shown.find("[user] prefs"), std::string::npos) << shown;
}

// 场景:TUI 里执行 /memory off,再 /memory on。期望:当前会话的记忆开关随之翻转。
TEST(TuiMemoryCommand, OffAndOnToggleCurrentSession) {
    MemoryTestHome home("tui-memory-toggle");
    MemoryCommandHarness h(home);
    EXPECT_NE(h.dispatch("/memory off").find("Memory is off for this session"), std::string::npos);
    EXPECT_FALSE(h.sm_.memory_enabled());
    EXPECT_EQ(h.dispatch("/memory on"), "Memory is on for this session.");
    EXPECT_TRUE(h.sm_.memory_enabled());
}
