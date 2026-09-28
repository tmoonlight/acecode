#include "agent/transcript/transcript_queries.hpp"
#include <gtest/gtest.h>

// 重试只跳过不可见记录,不能把可见的 assistant 消息当作用户消息跳过。
TEST(AgentTranscriptQueries, FindsVisibleTailWithoutSkippingAssistant) {
    acecode::ChatMessage user, assistant, bookkeeping;
    user.role = "user";
    user.content = "question";
    assistant.role = "assistant";
    assistant.content = "answer";
    bookkeeping.is_meta = true;
    std::vector<acecode::ChatMessage> messages{user, assistant, bookkeeping};
    EXPECT_EQ(acecode::agent::detail::trailing_transcript_message(messages), &messages[1]);
    EXPECT_EQ(acecode::agent::detail::trailing_transcript_message(messages, true), &messages[0]);
    EXPECT_TRUE(acecode::agent::detail::is_transcript_bookkeeping(bookkeeping));
}
