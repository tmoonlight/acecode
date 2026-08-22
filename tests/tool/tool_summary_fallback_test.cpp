#include <gtest/gtest.h>

#include "tool/tool_executor.hpp"
#include "utils/encoding.hpp"

#include <unordered_set>

namespace {

acecode::ToolImpl make_unsummarized_tool(const std::string& name) {
    acecode::ToolImpl tool;
    tool.definition.name = name;
    tool.definition.description = "test";
    tool.definition.parameters = nlohmann::json::object();
    tool.execute = [](const std::string&, const acecode::ToolContext&) {
        return acecode::ToolResult{"ok", true};
    };
    return tool;
}

} // namespace

TEST(ToolSummaryFallback, UsesOrderedArgumentValuesAndGenericIdentity) {
    const auto summary = acecode::build_fallback_tool_summary(
        "glob",
        R"({"pattern":"**/*","path":"C:/repo","options":{"hidden":true}})");

    EXPECT_EQ(summary.verb, "Glob");
    EXPECT_EQ(summary.icon, "*");
    EXPECT_EQ(summary.object,
              std::string("**/* \xC2\xB7 C:/repo \xC2\xB7 ") +
                  R"({"hidden":true})");
    EXPECT_TRUE(summary.metrics.empty());
}

TEST(ToolSummaryFallback, NormalizesWhitespaceAndRedactsSensitiveValues) {
    const auto summary = acecode::build_fallback_tool_summary(
        "mcp_echo",
        R"({"text":"first\n  second","api_key":"private","nested":{"db_password":"also-private","value":7}})");

    EXPECT_EQ(summary.verb, "Mcp_echo");
    EXPECT_EQ(summary.object,
              std::string("first second \xC2\xB7 [REDACTED] \xC2\xB7 ") +
                  R"({"db_password":"[REDACTED]","value":7})");
    EXPECT_EQ(summary.object.find("private"), std::string::npos);
}

TEST(ToolSummaryFallback, TruncatesWithoutBreakingUtf8) {
    const std::string repeated(90, 'x');
    const auto summary = acecode::build_fallback_tool_summary(
        "vision_analyze",
        nlohmann::ordered_json({{"prompt", repeated + "\xE4\xB8\xAD"}}).dump());

    EXPECT_LE(summary.object.size(), 80u);
    EXPECT_TRUE(acecode::is_valid_utf8(summary.object));
    EXPECT_EQ(summary.object.substr(summary.object.size() - 3), "...");
}

TEST(ToolSummaryFallback, HandlesEmptyMalformedArrayAndScalarArguments) {
    EXPECT_TRUE(acecode::build_fallback_tool_summary("glob", "{}").object.empty());
    EXPECT_TRUE(acecode::build_fallback_tool_summary("glob", "").object.empty());
    EXPECT_EQ(
        acecode::build_fallback_tool_summary("glob", "{bad   json").object,
        "{bad json");
    EXPECT_EQ(
        acecode::build_fallback_tool_summary(
            "mcp_batch", R"([1,"two",{"token":"private","ok":true}])").object,
        R"([1,"two",{"token":"[REDACTED]","ok":true}])");
    EXPECT_EQ(
        acecode::build_fallback_tool_summary("mcp_scalar", "42").object,
        "42");
}

TEST(ToolSummaryFallback, PreservesDomainSpecificSummary) {
    acecode::ToolResult result{"ok", true};
    acecode::ToolSummary custom;
    custom.verb = "Read";
    custom.object = "README.md";
    custom.icon = "R";
    result.summary = custom;

    acecode::ensure_tool_summary("file_read", R"({"file_path":"other"})", result);

    ASSERT_TRUE(result.summary.has_value());
    EXPECT_EQ(result.summary->verb, "Read");
    EXPECT_EQ(result.summary->object, "README.md");
    EXPECT_EQ(result.summary->icon, "R");
}

TEST(ToolSummaryFallback, ExecutorCoversSuccessUnknownAndPolicyDeniedResults) {
    acecode::ToolExecutor tools;
    ASSERT_TRUE(tools.register_tool(make_unsummarized_tool("glob")));

    const auto success = tools.execute(
        "glob", R"({"pattern":"*.cpp","path":"C:/repo"})");
    ASSERT_TRUE(success.summary.has_value());
    EXPECT_EQ(success.summary->verb, "Glob");
    EXPECT_EQ(success.summary->object,
              std::string("*.cpp \xC2\xB7 C:/repo"));

    const auto unknown = tools.execute("unknown_tool", R"({"value":1})");
    EXPECT_FALSE(unknown.success);
    ASSERT_TRUE(unknown.summary.has_value());
    EXPECT_EQ(unknown.summary->verb, "Unknown_tool");
    EXPECT_EQ(unknown.summary->object, "1");

    acecode::ToolCapabilityPolicy policy;
    policy.builtin_tools = std::unordered_set<std::string>{};
    acecode::ToolContext context;
    context.capability_policy = policy;
    const auto denied = tools.execute("glob", R"({"pattern":"*.hpp"})", context);
    EXPECT_FALSE(denied.success);
    ASSERT_TRUE(denied.summary.has_value());
    EXPECT_EQ(denied.summary->verb, "Glob");
    EXPECT_EQ(denied.summary->object, "*.hpp");
}

TEST(ToolSummaryFallback, ExecutorCoversDynamicMcpAndPreservesSpecialSummary) {
    acecode::ToolExecutor tools;
    auto mcp = make_unsummarized_tool("mcp_echo");
    mcp.source = acecode::ToolSource::Mcp;
    mcp.source_owner = "echo-server";
    ASSERT_TRUE(tools.register_tool(mcp));

    auto specialized = make_unsummarized_tool("custom_read");
    specialized.execute = [](const std::string&, const acecode::ToolContext&) {
        acecode::ToolResult result{"ok", true};
        acecode::ToolSummary summary;
        summary.verb = "Read";
        summary.object = "special.txt";
        summary.icon = "R";
        result.summary = summary;
        return result;
    };
    ASSERT_TRUE(tools.register_tool(specialized));

    const auto mcp_result = tools.execute(
        "mcp_echo", R"({"message":"hello","count":2})");
    ASSERT_TRUE(mcp_result.summary.has_value());
    EXPECT_EQ(mcp_result.summary->verb, "Mcp_echo");
    EXPECT_EQ(mcp_result.summary->object,
              std::string("hello \xC2\xB7 2"));

    const auto special_result = tools.execute(
        "custom_read", R"({"path":"ignored.txt"})");
    ASSERT_TRUE(special_result.summary.has_value());
    EXPECT_EQ(special_result.summary->verb, "Read");
    EXPECT_EQ(special_result.summary->object, "special.txt");
    EXPECT_EQ(special_result.summary->icon, "R");
}

TEST(ToolSummaryFallback, PersistsExplicitSuccessState) {
    acecode::ToolResult result{"failed", false};
    const auto message = acecode::ToolExecutor::format_tool_result(
        "call-1", result);

    ASSERT_TRUE(message.metadata.is_object());
    ASSERT_TRUE(message.metadata.contains("tool_success"));
    EXPECT_FALSE(message.metadata["tool_success"].get<bool>());
}
