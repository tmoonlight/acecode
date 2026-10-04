#include <gtest/gtest.h>
#include "agent/request/api_request_builder.hpp"
#include "agent/request/prompt_context_cache.hpp"
#include "skills/skill_activation.hpp"

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

TEST(ApiRequestBuilder, DeferredSkillToolsStaySessionLocalAndRespectPolicyAfterReload) {
    acecode::ToolExecutor tools;
    acecode::ToolImpl tool;
    tool.definition.name = "create_scheduled_task";
    tool.activation_skill = "scheduled-task";
    ASSERT_TRUE(tools.register_tool(tool));
    acecode::agent::PromptContextCache cache;
    acecode::agent::ApiRequestBuilder builder(tools, cache);
    acecode::agent::RequestContextOptions options;
    options.cwd = testing::TempDir();
    options.git_config.emplace();
    options.git_config->enabled = false;
    options.project_config.emplace();
    options.project_config->enabled = false;
    EXPECT_TRUE(builder.capture(options, {}, false).tool_defs.empty());
    EXPECT_EQ(tools.get_registered_tools().size(), 1u);

    acecode::ToolResult loaded{"skill instructions", true};
    loaded.metadata[acecode::kLoadedSkillsMetadata] = {"scheduled-task"};
    auto message = acecode::ToolExecutor::format_tool_result("load-skill", loaded);
    EXPECT_EQ(builder.capture(options, {message}, false).tool_defs.size(), 1u);
    EXPECT_EQ(builder.capture(options, {message}, false).builtin_tool_defs.size(), 1u);
    // A shared registry does not activate another session or a compacted history.
    EXPECT_TRUE(builder.capture(options, {}, false).tool_defs.empty());
    EXPECT_TRUE(tools.get_model_tool_definitions().empty());
    EXPECT_EQ(builder.capture(options, {message}, false).tool_defs.size(), 1u);

    message.metadata["tool_success"] = false;
    EXPECT_TRUE(builder.capture(options, {message}, false).tool_defs.empty());
    // Explicit Skill expansion uses a user-message load record.
    message.role = "user";
    EXPECT_EQ(builder.capture(options, {message}, false).tool_defs.size(), 1u);
    options.tool_policy.builtin_tools = std::unordered_set<std::string>{};
    EXPECT_TRUE(builder.capture(options, {message}, false).tool_defs.empty());
}
