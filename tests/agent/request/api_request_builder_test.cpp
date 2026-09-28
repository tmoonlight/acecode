#include <gtest/gtest.h>
#include "agent/request/api_request_builder.hpp"
#include "agent/request/prompt_context_cache.hpp"

TEST(PromptContextCache, CwdChangePreservesSkillAndSessionPins) {
    // cwd 失效只针对 Git;相同内容键下重采的动态文本仍钉住原字节。
    acecode::agent::PromptContextCache cache;
    EXPECT_EQ(cache.skills({"skill-first", "skill-key", ""}), "skill-first");
    EXPECT_EQ(cache.session({"session-first", "session-key", ""}), "session-first");
    cache.reset_on_cwd_change();
    EXPECT_EQ(cache.skills({"skill-new", "skill-key", ""}), "skill-first");
    EXPECT_EQ(cache.session({"session-new", "session-key", ""}), "session-first");
    EXPECT_EQ(cache.session({"session-new", "next-key", ""}), "session-new");
}

TEST(ApiRequestBuilder, MainAndCompactionUseIdenticalStaticPrefixAndCapturedInputs) {
    acecode::ToolExecutor tools;
    acecode::agent::PromptContextCache cache;
    acecode::agent::ApiRequestBuilder builder(tools, cache);
    acecode::agent::RequestContextOptions options;
    options.cwd = testing::TempDir();
    options.git_config.emplace();
    options.git_config->enabled = false;
    options.project_config.emplace();
    options.project_config->enabled = false;
    options.memory_config.emplace();
    options.memory_config->enabled = false;
    options.loop_active = true;
    options.loop_context = "fixed loop rule";
    const auto compact = builder.initial_context(options);
    acecode::ChatMessage user;
    user.role = "user";
    user.content = "request";
    auto inputs = builder.capture(options, {user}, false);
    options.loop_context = "changed after capture";
    inputs.hook_context = "hook snapshot";
    const auto request = builder.build(std::move(inputs));
    ASSERT_FALSE(compact.empty());
    ASSERT_FALSE(request.messages_with_system.empty());
    EXPECT_EQ(request.messages_with_system.front().content, compact.front().content);
    EXPECT_NE(request.messages_with_system.front().content.find("fixed loop rule"), std::string::npos);
    EXPECT_EQ(request.messages_with_system.front().content.find("changed after capture"), std::string::npos);
}
