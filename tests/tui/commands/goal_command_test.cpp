#include "test_support/agent/agent_loop_fixture.hpp"
#include <gtest/gtest.h>

#include "agent/agent_loop.hpp"
#include "tui/commands/command_registry.hpp"
#include "tui/commands/goal_command.hpp"
#include "config/config.hpp"
#include "permissions/permissions.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/thread_goal_store.hpp"
#include "tool/goal_tool.hpp"
#include "tool/tool_executor.hpp"
#include "session/token_tracker.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <thread>

namespace fs = std::filesystem;

namespace {

fs::path temp_cwd(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_goal_command_" + hint + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::remove_all(acecode::SessionStorage::get_project_dir(dir.string()));
    return dir;
}

class GoalCommandHarness {
public:
    explicit GoalCommandHarness(const std::string& hint)
        : cwd_(temp_cwd(hint))
        , loop_(std::make_unique<acecode::AgentLoop>(
        acecode_test::AgentLoopFixture::dependencies([] { return std::shared_ptr<acecode::LlmProvider>{}; }, tools_, acecode::AgentCallbacks{}, perms_, sm_.get()),
        acecode_test::AgentLoopFixture::configuration(cwd_.string()))) {
        loop_->start();
        // These tests exercise command parsing/state updates, not unattended
        // continuation. Plan mode keeps maybe_continue_goal() idle and avoids
        // racing /goal pause against a background turn on faster CI hosts.
        perms_.set_mode(acecode::PermissionMode::Plan);
        sm_->start_session(cwd_.string(), "stub", "model", "sid-" + hint);
        acecode::register_goal_command(registry_);
    }

    ~GoalCommandHarness() {
        loop_->shutdown();
        loop_.reset();
        sm_.reset();
        fs::remove_all(cwd_);
        fs::remove_all(acecode::SessionStorage::get_project_dir(cwd_.string()));
    }

    acecode::CommandContext context() {
        acecode::CommandContext ctx{
            state_,
            *loop_,
            nullptr,
            config_,
            tracker_,
            perms_,
        };
        ctx.session_manager = sm_.get();
        ctx.tools = &tools_;
        ctx.command_registry = &registry_;
        ctx.cwd = cwd_.string();
        return ctx;
    }

    bool dispatch(const std::string& text) {
        auto ctx = context();
        return registry_.dispatch(text, ctx);
    }

    std::optional<acecode::ThreadGoal> goal() {
        return sm_->goal_store()->get_thread_goal(sm_->current_session_id());
    }

    acecode::TuiState state_;
    acecode::CommandRegistry registry_;
    std::unique_ptr<acecode::SessionManager> sm_ =
        std::make_unique<acecode::SessionManager>();
    acecode::ToolExecutor tools_;
    acecode::PermissionManager perms_;
    acecode::AppConfig config_;
    acecode::TokenTracker tracker_;
    fs::path cwd_;
    std::unique_ptr<acecode::AgentLoop> loop_;
};

} // namespace

TEST(GoalCommand, CreatesEditsPausesAndClearsGoal) {
    GoalCommandHarness h("flow");

    ASSERT_TRUE(h.dispatch("/goal --tokens 50K finish migration"));
    auto goal = h.goal();
    ASSERT_TRUE(goal.has_value());
    EXPECT_EQ(goal->objective, "finish migration");
    EXPECT_EQ(goal->token_budget, 50000);
    EXPECT_EQ(goal->status, acecode::ThreadGoalStatus::Active);
    EXPECT_NE(h.state_.goal_status.find("active"), std::string::npos);

    ASSERT_TRUE(h.dispatch("/goal edit --tokens 60K finish all tests"));
    goal = h.goal();
    ASSERT_TRUE(goal.has_value());
    EXPECT_EQ(goal->objective, "finish all tests");
    EXPECT_EQ(goal->token_budget, 60000);

    ASSERT_TRUE(h.dispatch("/goal pause"));
    goal = h.goal();
    ASSERT_TRUE(goal.has_value());
    EXPECT_EQ(goal->status, acecode::ThreadGoalStatus::Paused);
    EXPECT_NE(h.state_.goal_status.find("paused"), std::string::npos);

    ASSERT_TRUE(h.dispatch("/goal clear"));
    goal = h.goal();
    EXPECT_FALSE(goal.has_value());
    EXPECT_TRUE(h.state_.goal_status.empty());
}

TEST(GoalCommand, RejectsInvalidBudget) {
    GoalCommandHarness h("invalid");

    ASSERT_TRUE(h.dispatch("/goal --tokens nope finish"));
    auto goal = h.goal();
    EXPECT_FALSE(goal.has_value());
    ASSERT_FALSE(h.state_.conversation.empty());
    EXPECT_NE(h.state_.conversation.back().content.find("positive integer"), std::string::npos);
}

// 触发场景:上一回合被用户中断后会话空闲(abort 标志还留着),用户在 TUI 里
// /goal 新建目标。期望行为:新目标立即开出续跑回合;本用例 provider 为空,
// 回合一跑就报「没有模型」并把目标转成 blocked,以此证明回合确实开了。
// 回归:修复前创建路径不清残留的 abort 标志,maybe_continue 直接返回,
// 目标一直停在 active,什么也不做。
TEST(GoalCommand, CreateAfterStaleAbortStartsContinuation) {
    GoalCommandHarness h("stale_abort_create");
    // 续跑要求 update_goal 可用;夹具默认工具表是空的。
    ASSERT_TRUE(h.tools_.register_tool(acecode::create_update_goal_tool()));
    h.perms_.set_mode(acecode::PermissionMode::Default);
    h.loop_->abort();

    ASSERT_TRUE(h.dispatch("/goal second goal"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<acecode::ThreadGoal> goal;
    while (std::chrono::steady_clock::now() < deadline) {
        goal = h.goal();
        if (goal.has_value() && goal->status == acecode::ThreadGoalStatus::Blocked) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(goal.has_value());
    EXPECT_EQ(goal->objective, "second goal");
    EXPECT_EQ(goal->status, acecode::ThreadGoalStatus::Blocked);
}

TEST(GoalCommand, ResumeBlockedGoalMakesItActive) {
    GoalCommandHarness h("blocked_resume");

    const std::string sid = h.sm_->current_session_id();
    ASSERT_TRUE(h.sm_->goal_store()->replace_thread_goal(
        sid, "wait for a decision", std::nullopt, acecode::ThreadGoalStatus::Blocked));

    ASSERT_TRUE(h.dispatch("/goal resume"));
    auto goal = h.goal();
    ASSERT_TRUE(goal.has_value());
    EXPECT_EQ(goal->status, acecode::ThreadGoalStatus::Active);
    EXPECT_NE(h.state_.goal_status.find("active"), std::string::npos);
}
