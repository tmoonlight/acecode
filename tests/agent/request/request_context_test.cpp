#include "agent/request/request_context.hpp"
#include <gtest/gtest.h>

using namespace acecode;
namespace detail = acecode::agent::detail;

// 同一个 cache_key 下内容即使重采也应钉住,防止回合中的 prompt cache 前缀漂移。
TEST(AgentRequestContext, PinsBytesUntilKeyChanges) {
    PromptContextBlock block;
    block.cache_key = "first";
    block.content = "original\n";
    std::string key, content;
    EXPECT_EQ(detail::cached_context_for_api(block, key, content), "original\n");
    block.content = "changed\n";
    EXPECT_EQ(detail::cached_context_for_api(block, key, content), "original\n");
    block.cache_key = "second";
    EXPECT_EQ(detail::cached_context_for_api(block, key, content), "changed\n");
}

// 空上下文不添加消息;plan 元信息与普通请求上下文必须保持不同。
TEST(AgentRequestContext, PreservesContextOrderAndMetadata) {
    std::vector<ChatMessage> messages;
    detail::append_plan_mode_context_for_api(messages, "");
    detail::append_request_context_for_api(messages, "");
    EXPECT_TRUE(messages.empty());
    detail::append_plan_mode_context_for_api(messages, "plan");
    detail::append_request_context_for_api(messages, "request");
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0].content, "plan");
    EXPECT_TRUE(messages[0].metadata.value("hidden_plan_mode_context", false));
    EXPECT_EQ(messages[1].content, "request");
    EXPECT_FALSE(messages[1].metadata.contains("hidden_plan_mode_context"));
}
