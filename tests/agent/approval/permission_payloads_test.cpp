#include "agent/approval/permission_payloads.hpp"
#include "tool/question_policy.hpp"
#include "config/config.hpp"
#include <gtest/gtest.h>

namespace detail = acecode::agent::detail;

// 非对象与损坏的 JSON 都保留原始字节,不能丢掉用户即将确认的参数。
TEST(AgentPermissionPayloads, RetainsMalformedAndScalarArguments) {
    EXPECT_EQ(detail::parse_tool_args_for_permission_payload(""), nlohmann::json::object());
    for (const auto& text : {"[1,2]", "broken", "null"}) {
        EXPECT_EQ(detail::parse_tool_args_for_permission_payload(text), nlohmann::json({{"raw", text}}));
    }
    EXPECT_EQ(detail::build_plan_permission_args("file_write", "broken", nullptr), "broken");
    auto enter = nlohmann::json::parse(detail::build_plan_permission_args("EnterPlanMode", "{}", nullptr));
    EXPECT_EQ(enter["kind"], "enter_plan_mode");
}

// CLI 选择优先,但未携带秒数时继续使用配置值;非显式配置仍回到 Ask。
TEST(AgentQuestionPolicy, SharesCliPrecedenceAcrossHosts) {
    acecode::AgentLoopConfig config;
    config.question_policy = "deny";
    config.question_policy_explicit = true;
    config.question_timeout_seconds = 91;
    config.question_policy_cli = "timeout";
    config.question_timeout_seconds_cli = 0;
    auto policy = acecode::resolve_question_policy(config);
    EXPECT_EQ(policy.policy, acecode::QuestionPolicy::Timeout);
    EXPECT_EQ(policy.timeout_seconds, 91);
    config.question_timeout_seconds_cli = 17;
    EXPECT_EQ(acecode::resolve_question_policy(config).timeout_seconds, 17);
    config.question_policy_cli.clear();
    config.question_policy_explicit = false;
    EXPECT_EQ(acecode::resolve_question_policy(config).policy, acecode::QuestionPolicy::Ask);
}
