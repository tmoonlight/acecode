#include <gtest/gtest.h>
#include "tui/app/startup_worktree.hpp"
#include "tui/app/interactive_options.hpp"
#include "session/session_manager.hpp"
#include "tool/worktree_tool.hpp"
#include "worktree/worktree_manager.hpp"
#include "utils/utf8_path.hpp"
#include "test_support/agent_loop/characterization_fixture.hpp"
#include <filesystem>
#include <fstream>

namespace {
namespace fs = std::filesystem;
using namespace acecode;
class StartupWorktreeTest : public testing::Test {
protected:
    acecode_test::characterization::Isolation isolation;
    acecode_test::characterization::TemporaryDirectory workspace;
    const fs::path saved_cwd = fs::current_path();
    const fs::path repo = workspace.path / "repo";
    std::string repo_utf8;
    SessionManager session;
    ToolContext context;
    ToolImpl enter;
    void SetUp() override {
        if (!worktree::run_git({"--version"}, "").ok()) GTEST_SKIP() << "git unavailable";
        fs::create_directories(repo);
        repo_utf8 = path_to_utf8(repo);
        ASSERT_TRUE(worktree::run_git({"init", "-b", "main"}, repo_utf8).ok());
        ASSERT_TRUE(worktree::run_git({"config", "user.email", "test@acecode.local"}, repo_utf8).ok());
        ASSERT_TRUE(worktree::run_git({"config", "user.name", "acecode-test"}, repo_utf8).ok());
        { std::ofstream out(repo / "README.md"); out << "baseline\n"; }
        ASSERT_TRUE(worktree::run_git({"add", "README.md"}, repo_utf8).ok());
        ASSERT_TRUE(worktree::run_git({"commit", "-m", "initial"}, repo_utf8).ok());
        session.start_session(repo_utf8, "test", "test");
        context.cwd = repo_utf8;
        context.session_manager = &session;
        auto cwd = std::make_shared<std::string>(repo_utf8);
        context.switch_session_cwd = [cwd](const std::string& value) { *cwd = value; };
        enter = create_enter_worktree_tool(WorktreeConfig{});
    }
    void TearDown() override {
        std::error_code error;
        fs::current_path(saved_cwd, error);
    }
};
}

TEST_F(StartupWorktreeTest, ToolEnteredCleanWorktreeIsRemovedOnExit) {
    // 没有 --worktree 启动状态时,EnterWorktree 创建的干净工作树也必须收尾。
    const auto result = enter.execute(R"({"name":"tool-entered"})", context);
    ASSERT_TRUE(result.success) << result.output;
    const auto info = session.active_worktree();
    const auto path = path_from_utf8(info.worktree_path);
    fs::current_path(path);
    tui::finalize_session_worktree_on_exit(session);
    EXPECT_FALSE(session.active_worktree().active());
    EXPECT_FALSE(fs::exists(path));
    EXPECT_TRUE(fs::equivalent(fs::current_path(), repo));
}

TEST_F(StartupWorktreeTest, DirtyWorktreeRemainsResumable) {
    // 未提交的用户文件必须留下,会话 worktree 信息也不能清空。
    ASSERT_TRUE(enter.execute(R"({"name":"dirty"})", context).success);
    const auto info = session.active_worktree();
    const auto path = path_from_utf8(info.worktree_path);
    { std::ofstream out(path / "wip.txt"); out << "user work\n"; }
    tui::finalize_session_worktree_on_exit(session);
    EXPECT_TRUE(session.active_worktree().active());
    EXPECT_EQ(session.active_worktree().worktree_path, info.worktree_path);
    EXPECT_TRUE(fs::exists(path / "wip.txt"));
}

TEST_F(StartupWorktreeTest, UnknownChangeStatePreservesSessionMetadata) {
    // 无法统计变更时保留元数据,不能把未知状态当作零变更。
    WorktreeSessionInfo info;
    info.original_cwd = repo_utf8;
    info.worktree_path = path_to_utf8(workspace.path / "missing");
    info.worktree_branch = "worktree-missing";
    info.original_head_commit = "unknown";
    session.set_active_worktree(info);
    tui::finalize_session_worktree_on_exit(session);
    EXPECT_TRUE(session.active_worktree().active());
    EXPECT_EQ(session.active_worktree().worktree_path, info.worktree_path);
}

TEST_F(StartupWorktreeTest, InvalidStartupSlugDoesNotChangeWorkingDirectory) {
    // 非法 slug 在任何创建或 chdir 前拒绝。
    InteractiveCliOptions options;
    options.worktree_name = "../escape";
    std::string cwd = repo_utf8, banner;
    WorktreeSessionInfo info;
    EXPECT_FALSE(tui::bootstrap_startup_worktree(options, cwd, info, banner));
    EXPECT_EQ(cwd, repo_utf8);
    EXPECT_FALSE(info.active());
    EXPECT_TRUE(fs::equivalent(fs::current_path(), saved_cwd));
}
