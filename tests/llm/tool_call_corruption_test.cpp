// 覆盖 src/domain/llm/tool_call_corruption.{hpp,cpp}:损坏工具调用判定。
//
// 背景:内网 wizard-ai 网关(PingAnCoder code_pilot)的 aicoder-pro = GLM 5.2 FP8,
// 高峰期没能把模型写的 GLM 工具调用模板(`<tool_call>名字` + `<arg_key>` /
// `<arg_value>`)转成结构化 tool_calls。2026-10-14 daemon 日志里返回的函数名原样
// 收在 kGlm* 常量里,作为回归样例。
//   1. 工具名:空名字、调用表达式、夹着模板标记的名字算损坏;格式正常但不存在的
//      名字不算(那走 Unknown tool)。
//   2. 参数:只认模板残片的结构,编辑含这些标记的文件不能被误伤。
//   3. find_corrupted_tool_call:已注册的名字跳过名字判定,参数照查。

#include <gtest/gtest.h>

#include "llm/llm_provider.hpp"
#include "llm/tool_call_corruption.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

using acecode::ToolCallCorruption;

// 2026-10-14 内网 daemon 日志里的真实坏名字。
const char* const kGlmNameWithClosingTag = "skill_view(name=\"kimi-webbridge\")</arg_value>";
const char* const kGlmChainedCalls =
    "skill_view(name=\"SDD-design-analysis\")<tool_call>skill_view(name=\"kimi-webbridge\")"
    "<tool_call>skill_view(name=\"SDD-requirement-analysis\")";
const char* const kGlmChainedBrackets =
    "skill_view[\"SDD-design-analysis\"]<tool_call>skill_view[\"kimi-webbridge\"]"
    "<tool_call>skill_view[\"SDD-requirement-analysis\"]";

acecode::ToolCall make_call(std::string name, std::string args = "{}") {
    acecode::ToolCall call;
    call.id = "call-" + name;
    call.function_name = std::move(name);
    call.function_arguments = std::move(args);
    return call;
}

} // namespace

// 场景:网关返回的函数名就是模型照着提示词写的伪函数调用,后面还拖着模板闭合
// 标签,或者几段调用被串进同一个名字里(日志原样)。
// 期望:都判为工具名损坏(TemplateMarkup),marker 是名字里最早出现的模板标记。
// 回归:旧实现只扫正文,这些名字被当成 Unknown tool 执行并写回历史。
TEST(ToolCallCorruption, RealGlmGatewayNamesAreCorrupted) {
    const auto closing = acecode::diagnose_corrupted_tool_name(kGlmNameWithClosingTag);
    ASSERT_TRUE(closing.has_value());
    EXPECT_EQ(closing->field, ToolCallCorruption::Field::Name);
    EXPECT_EQ(closing->reason, ToolCallCorruption::Reason::TemplateMarkup);
    EXPECT_EQ(closing->marker, "</arg_value>");

    for (const char* name : {kGlmChainedCalls, kGlmChainedBrackets}) {
        const auto chained = acecode::diagnose_corrupted_tool_name(name);
        ASSERT_TRUE(chained.has_value()) << name;
        EXPECT_EQ(chained->reason, ToolCallCorruption::Reason::TemplateMarkup) << name;
        EXPECT_EQ(chained->marker, "<tool_call>") << name;
    }
}

// 场景:日志里的第四种形态 —— 函数名为空、参数 {},finish_reason=stop(provider
// 打出 "synthesized missing tool_call id index=0 tool=''");另含只有空白的名字。
// 期望:判为 EmptyName,marker 为空。
TEST(ToolCallCorruption, EmptyOrBlankNameIsCorrupted) {
    for (const char* name : {"", " ", "\n\t "}) {
        const auto corruption = acecode::diagnose_corrupted_tool_name(name);
        ASSERT_TRUE(corruption.has_value()) << "[" << name << "]";
        EXPECT_EQ(corruption->reason, ToolCallCorruption::Reason::EmptyName);
        EXPECT_TRUE(corruption->marker.empty());
    }
}

// 场景:名字里没有模板标记,但明显是调用表达式(圆括号 / 方括号 / 引号 / 等号)。
// 期望:判为 CallSyntax,marker 是第一个调用语法字符。
TEST(ToolCallCorruption, CallExpressionNameIsCorrupted) {
    const auto paren = acecode::diagnose_corrupted_tool_name("skill_view(name=\"kimi-webbridge\")");
    ASSERT_TRUE(paren.has_value());
    EXPECT_EQ(paren->reason, ToolCallCorruption::Reason::CallSyntax);
    EXPECT_EQ(paren->marker, "(");

    const auto bracket = acecode::diagnose_corrupted_tool_name("skill_view[\"x\"]");
    ASSERT_TRUE(bracket.has_value());
    EXPECT_EQ(bracket->marker, "[");

    EXPECT_TRUE(acecode::diagnose_corrupted_tool_name("memory_read name=x").has_value());
}

// 场景:格式正常的名字(含 MCP 风格前缀),以及格式不规范但不像调用表达式的
// 名字(大小写不对、带点的命名空间前缀、带空格、超过 64 字符)。
// 期望:都不算损坏 —— 这些照旧走 Unknown tool,报错会列出可用工具让模型照抄;
// 只有空名字和调用表达式 / 模板残片才整步丢弃重发。
TEST(ToolCallCorruption, WellFormedOrMerelyUnknownNamesAreNotCorrupted) {
    for (const char* name : {"file_read", "AskUserQuestion", "mcp_srv_list-items", "Bash",
                             "functions.skill_view", "skill view", "server/tool"}) {
        EXPECT_FALSE(acecode::diagnose_corrupted_tool_name(name).has_value()) << name;
    }
    const std::string long_name(80, 'a');
    EXPECT_FALSE(acecode::is_conventional_tool_name(long_name));
    EXPECT_FALSE(acecode::diagnose_corrupted_tool_name(long_name).has_value());

    EXPECT_TRUE(acecode::is_conventional_tool_name("skill_view"));
    EXPECT_TRUE(acecode::is_conventional_tool_name(std::string(64, 'x')));
    EXPECT_FALSE(acecode::is_conventional_tool_name(std::string(65, 'x')));
    EXPECT_FALSE(acecode::is_conventional_tool_name(""));
}

// 场景:参数里漏进了模板结构 —— 值以自己的闭合标签结尾、相邻参数的
// `<arg_key>…</arg_key><arg_value>` 被吞进前一个值(上一个值缺闭合标签时
// 服务端正则的典型结果)、键名里带标签、嵌套对象里的值、非 JSON 的模板原文。
// 期望:全部判为参数损坏,marker 是结构开头的标记。
TEST(ToolCallCorruption, TemplateStructureInArgumentsIsCorrupted) {
    const auto expect_args = [](const nlohmann::json& args, const char* marker) {
        const auto corruption = acecode::diagnose_corrupted_tool_arguments(args.dump());
        ASSERT_TRUE(corruption.has_value()) << args.dump();
        EXPECT_EQ(corruption->field, ToolCallCorruption::Field::Arguments);
        EXPECT_EQ(corruption->marker, marker) << args.dump();
    };
    expect_args({{"name", "kimi-webbridge</arg_value>"}}, "</arg_value>");
    expect_args({{"name", "kimi-webbridge</arg_value>\n"}}, "</arg_value>");
    expect_args({{"command", "ls -la\n<arg_key>timeout</arg_key>\n<arg_value>30"}}, "</arg_key>");
    expect_args({{"command", "ls</arg_value>\n<arg_key>timeout"}}, "</arg_value>");
    expect_args({{"path", "a.txt</arg_value></tool_call>"}}, "</arg_value>");
    expect_args({{"<arg_key>command", "ls"}}, "<arg_key>");
    const nlohmann::json edit = {{"new_string", "x</arg_value>"}};
    expect_args({{"edits", nlohmann::json::array({edit})}}, "</arg_value>");

    const auto raw = acecode::diagnose_corrupted_tool_arguments(
        "<arg_key>name</arg_key>\n<arg_value>kimi-webbridge</arg_value>");
    ASSERT_TRUE(raw.has_value());
    EXPECT_EQ(raw->marker, "<arg_key>");
}

// 场景:模型在编辑本来就含这些标记的文件 —— ACECode 自己的
// leaked_tool_markup.cpp 标记表、测试里的损坏样例常量、Hermes 格式的文档示例,
// 以及参数里只出现单个标记的普通文本。
// 期望:都不算损坏,照常执行。按子串拦会让这类编辑每次都被丢弃重发,
// 重发后照样命中,回合直接报错停下。
TEST(ToolCallCorruption, LegitimateContentMentioningMarkersIsNotCorrupted) {
    const std::vector<nlohmann::json> cases = {
        {{"old_string",
          "static constexpr std::string_view kMarkers[] = {\n"
          "    \"<arg_key>\", \"</arg_key>\", \"<arg_value>\", \"</arg_value>\"};"}},
        {{"new_string",
          "const char* const kCorruptedReply =\n"
          "    \" roots\\n# 3.3# 4</think>5 4}\\n40</arg_value>\";"}},
        {{"content", "<tool_call>{\"name\": \"bash\"}</tool_call>\n"}},
        {{"content", "bash\n<arg_key>command"}},
        {{"path", "README.md"}},
    };
    for (const auto& args : cases) {
        EXPECT_FALSE(acecode::diagnose_corrupted_tool_arguments(args.dump()).has_value())
            << args.dump();
    }
    EXPECT_FALSE(acecode::diagnose_corrupted_tool_arguments("").has_value());
    EXPECT_FALSE(acecode::diagnose_corrupted_tool_arguments("{}").has_value());
}

// 场景:同一步里先是一个正常调用,第二个是坏名字;另有一个已注册工具的名字
// 恰好带括号(MCP 工具名不受字符集约束),但它的参数漏进了模板结构。
// 期望:返回第一个损坏调用的下标;已注册的名字不按名字判,参数照查;
// is_registered 为空时只按名字本身判。
TEST(ToolCallCorruption, FindCorruptedToolCallSkipsRegisteredNamesButChecksArguments) {
    const std::vector<acecode::ToolCall> calls = {
        make_call("file_read", R"({"path":"README.md"})"),
        make_call(kGlmNameWithClosingTag),
    };
    const auto found = acecode::find_corrupted_tool_call(calls, nullptr);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->index, 1u);
    EXPECT_EQ(found->corruption.field, ToolCallCorruption::Field::Name);

    const auto registered = [](const std::string& name) { return name == "mcp_srv_odd(v2)"; };
    EXPECT_FALSE(acecode::find_corrupted_tool_call(
        {make_call("mcp_srv_odd(v2)", R"({"q":"x"})")}, registered).has_value());
    const auto bad_args = acecode::find_corrupted_tool_call(
        {make_call("mcp_srv_odd(v2)", R"({"q":"x</arg_value>"})")}, registered);
    ASSERT_TRUE(bad_args.has_value());
    EXPECT_EQ(bad_args->corruption.field, ToolCallCorruption::Field::Arguments);

    EXPECT_FALSE(acecode::find_corrupted_tool_call(
        {make_call("functions.noop"), make_call("file_read")}, nullptr).has_value());
}
