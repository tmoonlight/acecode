#include "agent/turn/user_turn_message.hpp"
#include "utils/text.hpp"
#include <gtest/gtest.h>

// 空白输入(包含纵向制表符)不启动回合;中文和非空文本必须保留。
TEST(AgentUserTurnMessage, UsesExistingWhitespaceRules) {
    acecode::UserInput input;
    input.text = " \t\r\n\v\f";
    EXPECT_FALSE(acecode::agent::detail::has_meaningful_user_input(input));
    EXPECT_EQ(acecode::utils::trim_ascii_copy(input.text), "");
    input.text = "  继续  ";
    EXPECT_TRUE(acecode::agent::detail::has_meaningful_user_input(input));
    EXPECT_EQ(acecode::utils::trim_ascii_copy(input.text), "继续");
}

// 旁路问题保留明确的只读语义,尾部问题原文不折叠、不裁剪。
TEST(AgentUserTurnMessage, KeepsSideQuestionReadOnlyAndLiteral) {
    const auto message = acecode::agent::detail::build_side_question_message("why?\n ");
    EXPECT_EQ(message.role, "user");
    EXPECT_NE(message.content.find("Do not call tools"), std::string::npos);
    EXPECT_NE(message.content.find("Side question:\nwhy?\n "), std::string::npos);
}
