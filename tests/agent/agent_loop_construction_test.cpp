#include <gtest/gtest.h>
#include "test_support/agent/agent_loop_fixture.hpp"
#include "test_support/agent_loop/characterization_fixture.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/permission_prompter.hpp"
#include "agent/model_step/active_model_view.hpp"
#include "pa/pa_context_budget.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
TEST(AgentLoopConstruction, NoTaskRunsUntilStartAndStartIsIdempotent) {
    // 先装配再启动:构造后的已排队 control 不能提前访问尚未安装的宿主依赖。
    acecode_test::characterization::Isolation isolation;
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode_test::AgentLoopFixture fixture({}, tools, {}, isolation.directory.path.string(), permissions);
    auto loop = fixture.make();
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto receipt = loop->enqueue_control([calls] { ++*calls; return true; });
    EXPECT_FALSE(receipt.completed());
    EXPECT_EQ(calls->load(), 0);
    loop->start();
    loop->start();
    ASSERT_TRUE(receipt.wait_for_completion(2s));
    EXPECT_TRUE(receipt.succeeded());
    EXPECT_EQ(calls->load(), 1);
    loop->shutdown();
    EXPECT_THROW(loop->start(), std::logic_error);
}
TEST(AgentLoopConstruction, BothPromptersBelongToLoopAndCannotBeReplacedAfterStart) {
    // prompter 引用 loop.events,必须在 start 前构建,并先于 events 析构。
    acecode_test::characterization::Isolation isolation;
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode_test::AgentLoopFixture fixture({}, tools, {}, isolation.directory.path.string(), permissions);
    auto loop = fixture.make();
    loop->set_permission_prompter(std::make_unique<acecode::AsyncPrompter>(loop->events()));
    loop->set_ask_question_prompter(std::make_unique<acecode::AskUserQuestionPrompter>(loop->events()));
    loop->start();
    EXPECT_THROW(loop->set_permission_prompter(nullptr), std::logic_error);
    EXPECT_THROW(loop->set_ask_question_prompter(nullptr), std::logic_error);
    loop->shutdown();
}
TEST(AgentLoopConstruction, RuntimeEnvironmentIsInjectedBeforePriming) {
    // 不改进程单例也能控制请求环境;尚未 start 时的 side-question prime 同样使用注入源。
    acecode_test::characterization::Isolation isolation;
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    acecode_test::AgentLoopFixture fixture([provider] { return provider; }, tools, {},
        isolation.directory.path.string(), permissions);
    fixture.services.runtime.prompt_environment = [] {
        acecode::SystemPromptEnvironment result;
        result.terminal_family = "powershell";
        result.terminal_program = "injected-shell-marker";
        return result;
    };
    auto loop = fixture.make();
    loop->prime_side_question_context();
    std::string prompt;
    for (const auto& message : loop->side_question_context_snapshot()) prompt += message.content;
    EXPECT_NE(prompt.find("injected-shell-marker"), std::string::npos);
}
TEST(AgentRuntimeEnvironment, ContextLearnerCanBeIsolatedFromProcessState) {
    // 同一个模型在两个租约域中的观测互不污染,避免测试改写全局 PA 学习器。
    auto learner = std::make_shared<acecode::pa::ContextBudgetLearner>();
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    acecode::AgentRuntimeEnv environment;
    environment.context_budget = [learner]() -> acecode::pa::ContextBudgetLearner& { return *learner; };
    acecode::agent::ActiveModelView model(provider, 128000, environment);
    EXPECT_EQ(model.effective_window(), 128000);
    EXPECT_FALSE(model.note_rejected(24000).has_value());
    EXPECT_EQ(model.effective_window(), 128000);
    const auto notice = model.note_rejected(46859);
    ASSERT_TRUE(notice.has_value());
    EXPECT_EQ(model.effective_window(), 39830);
    EXPECT_EQ(notice->metadata.at("system_notice").at("params").at("threshold"), 40000);
}
}
