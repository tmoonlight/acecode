#include <gtest/gtest.h>

#include "provider/dsml_tool_call_recovery.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

using acecode::DsmlToolCallStreamFilter;
using acecode::ToolDef;
using acecode::recover_dsml_tool_calls;

std::vector<ToolDef> test_tools() {
    ToolDef bash;
    bash.name = "bash";
    ToolDef read;
    read.name = "read";
    return {bash, read};
}

std::string single_bash_dsml() {
    return u8R"(<｜DSML｜tool_calls>
<｜DSML｜invoke name="bash">
<｜DSML｜parameter name="command" string="true">git status 2>&1</｜DSML｜parameter>
</｜DSML｜invoke>
</｜DSML｜tool_calls>)";
}

TEST(DsmlToolCallRecoveryTest, RecoversNarrationAndStringParameter) {
    const std::string input = "I will inspect the repository.\n" + single_bash_dsml();
    const auto result = recover_dsml_tool_calls(input, test_tools());

    ASSERT_TRUE(result.recovered) << result.error;
    EXPECT_EQ(result.visible_text, "I will inspect the repository.\n");
    ASSERT_EQ(result.tool_calls.size(), 1u);
    EXPECT_EQ(result.tool_calls[0].function_name, "bash");
    EXPECT_EQ(result.tool_calls[0].id.rfind("call_dsml_", 0), 0u);

    const auto arguments = nlohmann::json::parse(
        result.tool_calls[0].function_arguments);
    EXPECT_EQ(arguments["command"], "git status 2>&1");
}

TEST(DsmlToolCallRecoveryTest, RepeatedResponsesUseDistinctCallIds) {
    const std::string input = single_bash_dsml();
    const auto first = recover_dsml_tool_calls(input, test_tools());
    const auto second = recover_dsml_tool_calls(input, test_tools());

    ASSERT_TRUE(first.recovered) << first.error;
    ASSERT_TRUE(second.recovered) << second.error;
    ASSERT_EQ(first.tool_calls.size(), 1u);
    ASSERT_EQ(second.tool_calls.size(), 1u);
    EXPECT_NE(first.tool_calls[0].id, second.tool_calls[0].id);
}

TEST(DsmlToolCallRecoveryTest, RecoversMultipleCallsAndJsonValueTypes) {
    const std::string input = u8R"(<｜DSML｜tool_calls>
<｜DSML｜invoke name="bash">
<｜DSML｜parameter name="command" string="true">echo ok</｜DSML｜parameter>
<｜DSML｜parameter name="count" string="false">3</｜DSML｜parameter>
<｜DSML｜parameter name="enabled" string="false">true</｜DSML｜parameter>
<｜DSML｜parameter name="items" string="false">[1,"two"]</｜DSML｜parameter>
<｜DSML｜parameter name="config" string="false">{"depth":2}</｜DSML｜parameter>
<｜DSML｜parameter name="empty" string="false">null</｜DSML｜parameter>
</｜DSML｜invoke>
<｜DSML｜invoke name="read">
<｜DSML｜parameter name="path" string="true">README.md</｜DSML｜parameter>
</｜DSML｜invoke>
</｜DSML｜tool_calls><｜end▁of▁sentence｜>)";

    const auto result = recover_dsml_tool_calls(input, test_tools());
    ASSERT_TRUE(result.recovered) << result.error;
    ASSERT_EQ(result.tool_calls.size(), 2u);

    const auto first = nlohmann::json::parse(
        result.tool_calls[0].function_arguments);
    EXPECT_EQ(first["command"], "echo ok");
    EXPECT_EQ(first["count"], 3);
    EXPECT_EQ(first["enabled"], true);
    EXPECT_EQ(first["items"], nlohmann::json::array({1, "two"}));
    EXPECT_EQ(first["config"], nlohmann::json({{"depth", 2}}));
    EXPECT_TRUE(first["empty"].is_null());

    const auto second = nlohmann::json::parse(
        result.tool_calls[1].function_arguments);
    EXPECT_EQ(result.tool_calls[1].function_name, "read");
    EXPECT_EQ(second["path"], "README.md");
    EXPECT_NE(result.tool_calls[0].id, result.tool_calls[1].id);
}

TEST(DsmlToolCallRecoveryTest, HoldsMarkerAcrossEveryByteBoundary) {
    const std::string input = "before\n" + single_bash_dsml();
    DsmlToolCallStreamFilter filter(test_tools());
    std::string visible;

    for (std::size_t i = 0; i < input.size(); ++i) {
        visible += filter.push(std::string_view(input.data() + i, 1));
    }
    auto result = filter.finish();
    visible += result.visible_text;

    ASSERT_TRUE(result.recovered) << result.error;
    EXPECT_EQ(visible, "before\n");
    ASSERT_EQ(result.tool_calls.size(), 1u);
    EXPECT_EQ(result.tool_calls[0].function_name, "bash");
}

TEST(DsmlToolCallRecoveryTest, HidesIncompleteMarkerPrefixAtEnd) {
    const std::string input = u8"ordinary <｜DSML｜tool_call";
    DsmlToolCallStreamFilter filter(test_tools());
    std::string visible;
    for (char c : input) {
        visible += filter.push(std::string_view(&c, 1));
    }
    const auto result = filter.finish();
    visible += result.visible_text;

    EXPECT_FALSE(result.recovered);
    EXPECT_EQ(visible, "ordinary ");
    EXPECT_EQ(visible.find(u8"<｜DSML｜"), std::string::npos);
    EXPECT_TRUE(result.tool_calls.empty());
}

TEST(DsmlToolCallRecoveryTest, LeavesFencedExampleAsText) {
    const std::string input = "Example:\n```text\n" + single_bash_dsml() +
                              "\n```\n";
    const auto result = recover_dsml_tool_calls(input, test_tools());

    EXPECT_FALSE(result.recovered);
    EXPECT_EQ(result.visible_text, input);
    EXPECT_TRUE(result.tool_calls.empty());
}

TEST(DsmlToolCallRecoveryTest, RecoversAfterClosedMarkdownFence) {
    const std::string input = "```text\nnot a call\n```\n" + single_bash_dsml();
    const auto result = recover_dsml_tool_calls(input, test_tools());

    ASSERT_TRUE(result.recovered) << result.error;
    EXPECT_EQ(result.visible_text, "```text\nnot a call\n```\n");
    ASSERT_EQ(result.tool_calls.size(), 1u);
}

TEST(DsmlToolCallRecoveryTest, RejectsUnknownToolDuplicateParameterAndInvalidJson) {
    const std::vector<std::string> rejected = {
        u8R"(<｜DSML｜tool_calls><｜DSML｜invoke name="unknown"></｜DSML｜invoke></｜DSML｜tool_calls>)",
        u8R"(<｜DSML｜tool_calls><｜DSML｜invoke name="bash"><｜DSML｜parameter name="x" string="true">one</｜DSML｜parameter><｜DSML｜parameter name="x" string="true">two</｜DSML｜parameter></｜DSML｜invoke></｜DSML｜tool_calls>)",
        u8R"(<｜DSML｜tool_calls><｜DSML｜invoke name="bash"><｜DSML｜parameter name="x" string="false">not-json</｜DSML｜parameter></｜DSML｜invoke></｜DSML｜tool_calls>)",
    };

    for (const auto& input : rejected) {
        const auto result = recover_dsml_tool_calls(input, test_tools());
        EXPECT_FALSE(result.recovered) << input;
        EXPECT_TRUE(result.visible_text.empty()) << result.visible_text;
        EXPECT_EQ(result.visible_text.find(u8"<｜DSML｜"), std::string::npos);
        EXPECT_TRUE(result.tool_calls.empty());
        EXPECT_FALSE(result.error.empty());
    }
}

TEST(DsmlToolCallRecoveryTest, KeepsTrailingTextAfterValidWrapper) {
    const auto result = recover_dsml_tool_calls(
        single_bash_dsml() + " trailing note", test_tools());
    ASSERT_TRUE(result.recovered) << result.error;
    EXPECT_EQ(result.visible_text, " trailing note");
    ASSERT_EQ(result.tool_calls.size(), 1u);
}

TEST(DsmlToolCallRecoveryTest, FiltersSpecialTokensAfterWrapperWhitespace) {
    const auto result = recover_dsml_tool_calls(
        single_bash_dsml() +
            u8"\n<｜end▁of▁sentence｜>after<｜begin▁of▁sentence｜>",
        test_tools());

    ASSERT_TRUE(result.recovered) << result.error;
    EXPECT_EQ(result.visible_text, "\nafter");
    EXPECT_EQ(result.visible_text.find(u8"<｜"), std::string::npos);
    ASSERT_EQ(result.tool_calls.size(), 1u);
}

TEST(DsmlToolCallRecoveryTest, RecoversConsecutiveWrappersWithoutMarkupLeak) {
    const std::string read_wrapper = u8R"(<｜DSML｜tool_calls>
<｜DSML｜invoke name="read">
<｜DSML｜parameter name="path" string="true">README.md</｜DSML｜parameter>
</｜DSML｜invoke>
</｜DSML｜tool_calls>)";
    const auto result = recover_dsml_tool_calls(
        single_bash_dsml() + "\nbetween\n" + read_wrapper,
        test_tools());

    ASSERT_TRUE(result.recovered) << result.error;
    EXPECT_EQ(result.visible_text, "\nbetween\n");
    EXPECT_EQ(result.visible_text.find(u8"<｜DSML｜"), std::string::npos);
    ASSERT_EQ(result.tool_calls.size(), 2u);
    EXPECT_EQ(result.tool_calls[0].function_name, "bash");
    EXPECT_EQ(result.tool_calls[1].function_name, "read");
    EXPECT_NE(result.tool_calls[0].id, result.tool_calls[1].id);
}

TEST(DsmlToolCallRecoveryTest, HidesEmptyWrapper) {
    const auto result = recover_dsml_tool_calls(
        u8"<｜DSML｜tool_calls></｜DSML｜tool_calls>", test_tools());
    EXPECT_FALSE(result.recovered);
    EXPECT_TRUE(result.visible_text.empty());
    EXPECT_TRUE(result.tool_calls.empty());
}

TEST(DsmlToolCallRecoveryTest, StripsMarkupWhenRequestHasNoTools) {
    const auto result = recover_dsml_tool_calls(single_bash_dsml(), {});
    EXPECT_FALSE(result.recovered);
    EXPECT_TRUE(result.visible_text.empty());
    EXPECT_EQ(result.visible_text.find(u8"<｜DSML｜"), std::string::npos);
    EXPECT_TRUE(result.tool_calls.empty());
}

TEST(DsmlToolCallRecoveryTest, AcceptsNativeAndPublicToolNames) {
    ToolDef read;
    read.name = "read";
    const std::string input = u8R"(<｜DSML｜tool_calls>
<｜DSML｜invoke name="file_read">
<｜DSML｜parameter name="path" string="true">README.md</｜DSML｜parameter>
</｜DSML｜invoke>
</｜DSML｜tool_calls>)";
    const auto result = recover_dsml_tool_calls(input, {read});
    ASSERT_TRUE(result.recovered) << result.error;
    ASSERT_EQ(result.tool_calls.size(), 1u);
    EXPECT_EQ(result.tool_calls[0].function_name, "file_read");
}

TEST(DsmlToolCallRecoveryTest, RecoversMultilineStringParameter) {
    const std::string input = u8R"(<｜DSML｜tool_calls>
<｜DSML｜invoke name="bash">
<｜DSML｜parameter name="command" string="true">cd /tmp && \
echo one
echo two</｜DSML｜parameter>
</｜DSML｜invoke>
</｜DSML｜tool_calls>)";
    const auto result = recover_dsml_tool_calls(input, test_tools());
    ASSERT_TRUE(result.recovered) << result.error;
    const auto arguments = nlohmann::json::parse(
        result.tool_calls[0].function_arguments);
    EXPECT_EQ(arguments["command"], "cd /tmp && \\\necho one\necho two");
}

TEST(DsmlToolCallRecoveryTest, AcceptsParameterWithoutStringAttribute) {
    const std::string input = u8R"(<｜DSML｜tool_calls>
<｜DSML｜invoke name="bash">
<｜DSML｜parameter name="command">git status</｜DSML｜parameter>
</｜DSML｜invoke>
</｜DSML｜tool_calls>)";
    const auto result = recover_dsml_tool_calls(input, test_tools());
    ASSERT_TRUE(result.recovered) << result.error;
    const auto arguments = nlohmann::json::parse(
        result.tool_calls[0].function_arguments);
    EXPECT_EQ(arguments["command"], "git status");
}

TEST(DsmlToolCallRecoveryTest, StripsEndOfSentenceFromNarration) {
    const std::string input = u8"hello<｜end▁of▁sentence｜>world";
    const auto result = recover_dsml_tool_calls(input, test_tools());
    EXPECT_FALSE(result.recovered);
    EXPECT_EQ(result.visible_text, "helloworld");
}

TEST(DsmlToolCallRecoveryTest, ResetDropsCandidateFromPreviousAttempt) {
    DsmlToolCallStreamFilter filter(test_tools());
    const std::string partial = u8"<｜DSML｜tool_calls><｜DSML｜invoke name=\"bash\">";
    EXPECT_TRUE(filter.push(partial).empty());

    filter.reset();
    const std::string normal = "normal response after retry";
    std::string visible = filter.push(normal);
    const auto result = filter.finish();
    visible += result.visible_text;

    EXPECT_FALSE(result.recovered);
    EXPECT_EQ(visible, normal);
    EXPECT_TRUE(result.tool_calls.empty());
}

} // namespace
