#include "prompt/context_usage_breakdown.hpp"
#include "commands/compact.hpp"
#include "tool/tool_executor.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

acecode::ToolImpl make_tool(const std::string& name,
                            acecode::ToolSource source) {
    acecode::ToolImpl tool;
    tool.definition.name = name;
    tool.definition.description = "Description for " + name;
    tool.definition.parameters = {
        {"type", "object"},
        {"properties", {
            {"path", {{"type", "string"}}},
        }},
    };
    tool.source = source;
    tool.execute = [](const std::string&, const acecode::ToolContext&) {
        return acecode::ToolResult{"ok", true};
    };
    return tool;
}

TEST(ContextUsageBreakdownTest, EstimatesEveryPopulatedCategory) {
    acecode::ToolExecutor tools;
    tools.register_tool(make_tool("builtin_example", acecode::ToolSource::Builtin));
    tools.register_tool(make_tool("mcp_example", acecode::ToolSource::Mcp));

    acecode::ChatMessage conversation;
    conversation.role = "user";
    conversation.content = "A provider-visible conversation message.";

    acecode::ChatMessage mutable_context;
    mutable_context.role = "user";
    mutable_context.content =
        "rules12skills1dynamic-context-and-envelope";

    const auto result = acecode::estimate_context_usage_breakdown(
        "Static system prompt.",
        {conversation},
        {mutable_context},
        /*project_rules_bytes=*/8,
        /*skills_bytes=*/8,
        tools.get_tool_definitions_by_source(acecode::ToolSource::Builtin),
        tools.get_tool_definitions_by_source(acecode::ToolSource::Mcp));

    EXPECT_TRUE(result.has_data);
    EXPECT_GT(result.system_prompt, 0);
    EXPECT_GT(result.project_rules, 0);
    EXPECT_GT(result.skills, 0);
    EXPECT_GT(result.builtin_tools, 0);
    EXPECT_GT(result.mcp_tools, 0);
    EXPECT_GT(result.conversation, 0);
    EXPECT_GT(result.dynamic_context, 0);
}

TEST(ContextUsageBreakdownTest, LeavesAbsentOptionalCategoriesAtZero) {
    acecode::ChatMessage conversation;
    conversation.role = "user";
    conversation.content = "hello";

    const auto result = acecode::estimate_context_usage_breakdown(
        "system",
        {conversation},
        {},
        0,
        0,
        {},
        {});

    EXPECT_TRUE(result.has_data);
    EXPECT_EQ(result.project_rules, 0);
    EXPECT_EQ(result.skills, 0);
    EXPECT_EQ(result.builtin_tools, 0);
    EXPECT_EQ(result.mcp_tools, 0);
    EXPECT_EQ(result.dynamic_context, 0);
}

TEST(ContextUsageBreakdownTest, MutableMessagePartitionPreservesTotal) {
    acecode::ChatMessage mutable_context;
    mutable_context.role = "user";
    mutable_context.content = std::string(80, 'x');

    const auto result = acecode::estimate_context_usage_breakdown(
        "",
        {},
        {mutable_context},
        /*project_rules_bytes=*/24,
        /*skills_bytes=*/20,
        {},
        {});

    const int mutable_total =
        acecode::estimate_message_tokens({mutable_context});
    EXPECT_EQ(
        result.project_rules + result.skills + result.dynamic_context,
        mutable_total);
}

TEST(ContextUsageBreakdownTest, ReconcilesExactlyToProviderTotal) {
    acecode::ContextUsageBreakdown raw;
    raw.system_prompt = 100;
    raw.project_rules = 200;
    raw.skills = 300;
    raw.builtin_tools = 50;
    raw.mcp_tools = 100;
    raw.conversation = 150;
    raw.dynamic_context = 100;
    raw.has_data = true;

    const auto result =
        acecode::reconcile_context_usage_breakdown(raw, 1200);

    EXPECT_TRUE(result.has_data);
    EXPECT_EQ(acecode::context_usage_breakdown_total(result), 1200);
    EXPECT_EQ(result.system_prompt, 120);
    EXPECT_EQ(result.project_rules, 240);
    EXPECT_EQ(result.skills, 360);
}

TEST(ContextUsageBreakdownTest, LargestRemainderUsesStableCategoryOrder) {
    acecode::ContextUsageBreakdown raw;
    raw.system_prompt = 1;
    raw.project_rules = 1;
    raw.skills = 1;
    raw.builtin_tools = 1;
    raw.mcp_tools = 1;
    raw.conversation = 1;
    raw.dynamic_context = 1;

    const auto result =
        acecode::reconcile_context_usage_breakdown(raw, 3);

    EXPECT_EQ(result.system_prompt, 1);
    EXPECT_EQ(result.project_rules, 1);
    EXPECT_EQ(result.skills, 1);
    EXPECT_EQ(result.builtin_tools, 0);
    EXPECT_EQ(result.mcp_tools, 0);
    EXPECT_EQ(result.conversation, 0);
    EXPECT_EQ(result.dynamic_context, 0);
    EXPECT_EQ(acecode::context_usage_breakdown_total(result), 3);
}

TEST(ContextUsageBreakdownTest, EmptyWeightsFallBackToConversation) {
    const auto result = acecode::reconcile_context_usage_breakdown(
        acecode::ContextUsageBreakdown{},
        44100);

    EXPECT_TRUE(result.has_data);
    EXPECT_EQ(result.conversation, 44100);
    EXPECT_EQ(acecode::context_usage_breakdown_total(result), 44100);
}

TEST(ContextUsageBreakdownTest, MissingProviderUsageStaysUnavailable) {
    acecode::ContextUsageBreakdown raw;
    raw.system_prompt = 100;

    const auto result =
        acecode::reconcile_context_usage_breakdown(raw, 0);

    EXPECT_FALSE(result.has_data);
    EXPECT_EQ(acecode::context_usage_breakdown_total(result), 0);
}

TEST(ContextUsageBreakdownTest, JsonRoundTripAndLegacyDefaults) {
    acecode::ContextUsageBreakdown input;
    input.system_prompt = 478;
    input.project_rules = 4400;
    input.skills = 5200;
    input.builtin_tools = 9900;
    input.mcp_tools = 2200;
    input.conversation = 21000;
    input.dynamic_context = 922;
    input.has_data = true;

    const auto restored = acecode::context_usage_breakdown_from_json(
        acecode::context_usage_breakdown_to_json(input));
    EXPECT_TRUE(restored.has_data);
    EXPECT_EQ(restored.system_prompt, 478);
    EXPECT_EQ(restored.conversation, 21000);

    const auto legacy =
        acecode::context_usage_breakdown_from_json(nlohmann::json::object());
    EXPECT_FALSE(legacy.has_data);
    EXPECT_EQ(acecode::context_usage_breakdown_total(legacy), 0);
}

} // namespace
