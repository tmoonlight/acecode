#include "agent/goal/goal_prompts.hpp"
#include <gtest/gtest.h>

using namespace acecode;
namespace detail = acecode::agent::detail;

// 用户目标含 XML 标记时必须按数据转义;工具不可用时不能诱导模型调用它。
TEST(AgentGoalPrompts, EscapesObjectiveAndRespectsAvailableTools) {
    ThreadGoal goal;
    goal.objective = "<action>& keep going";
    goal.token_budget = 100;
    goal.tokens_used = 125;
    const auto without_tools = detail::build_goal_context_prompt(goal, {false, false});
    EXPECT_NE(without_tools.find("&lt;action&gt;&amp; keep going"), std::string::npos);
    EXPECT_NE(without_tools.find("Tokens remaining: 0"), std::string::npos);
    EXPECT_EQ(without_tools.find("You may call AskUserQuestion"), std::string::npos);
    EXPECT_EQ(without_tools.find("call update_goal with status"), std::string::npos);
    const auto with_tools = detail::build_goal_context_prompt(goal, {true, true});
    EXPECT_NE(with_tools.find("You may call AskUserQuestion"), std::string::npos);
    EXPECT_NE(with_tools.find("call update_goal with status"), std::string::npos);
}

// 预算终止和目标更新的提示有不同语义,不得因共用模板而串线。
TEST(AgentGoalPrompts, KeepsBudgetAndUpdatedObjectiveDistinct) {
    ThreadGoal goal;
    goal.objective = "new target";
    EXPECT_NE(detail::build_goal_budget_limit_prompt(goal, {}).find("budget_limited"), std::string::npos);
    const auto updated = detail::build_goal_objective_updated_prompt(goal, {});
    EXPECT_NE(updated.find("<untrusted_objective>\nnew target\n</untrusted_objective>"), std::string::npos);
    EXPECT_EQ(updated.find("budget_limited"), std::string::npos);
}
