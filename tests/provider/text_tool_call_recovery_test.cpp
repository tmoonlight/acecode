// 文本形式工具调用恢复(fix-feedback-0924 第 3 条)的纯逻辑测试。
//
// 现场:yubo2 会话(dots3-note-prev,OpenAI 兼容接口)里 14 条 assistant 消息把
// 工具调用写进正文 —— 裸 `<invoke name="Bash">`、`<dots_function_call>` 外壳、
// `<invoke name="exec">`(不存在的工具)—— provider 返回 tool_calls=0,
// AgentLoop 走 `Text-only response; ending loop` 静默结束回合;另有 3 条压缩
// 摘要只剩 `<dots_function_call>…`。这些形态的共同点:块前只有空白、块后没有
// 内容、参数值两侧各带一个换行。

#include <gtest/gtest.h>

#include "provider/text_tool_call_recovery.hpp"
#include "llm/tool_protocol_names.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using acecode::TextToolCallDiagnostic;
using acecode::TextToolCallRecoveryResult;
using acecode::TextToolCallStreamFilter;
using acecode::ToolCall;
using acecode::ToolDef;
using acecode::recover_text_tool_calls;
using Outcome = acecode::TextToolCallDiagnostic::Outcome;

ToolDef make_tool(std::string name, nlohmann::json properties) {
    ToolDef def;
    def.name = std::move(name);
    def.description = "test tool";
    def.parameters = {
        {"type", "object"},
        {"properties", std::move(properties)},
    };
    return def;
}

// 与现场一致:bash 的超时参数叫 timeout_ms(integer),不是 timeout。
std::vector<ToolDef> test_tools() {
    return {
        make_tool("bash", {
            {"command", {{"type", "string"}}},
            {"timeout_ms", {{"type", "integer"}}},
        }),
        make_tool("file_read", {
            {"file_path", {{"type", "string"}}},
        }),
    };
}

nlohmann::json args_of(const ToolCall& call) {
    return nlohmann::json::parse(call.function_arguments);
}

std::string bash_invoke(std::string_view name, std::string_view command) {
    return "<invoke name=\"" + std::string(name) +
           "\">\n<parameter name=\"command\">\n" + std::string(command) +
           "\n</parameter>\n</invoke>";
}

struct Streamed {
    std::string visible;
    TextToolCallRecoveryResult result;
};

Streamed stream_in_chunks(std::string_view input, const std::vector<ToolDef>& tools,
                          std::size_t chunk) {
    TextToolCallStreamFilter filter(tools);
    Streamed out;
    for (std::size_t i = 0; i < input.size(); i += chunk) {
        out.visible += filter.push(input.substr(i, chunk));
    }
    out.result = filter.finish();
    out.visible += out.result.visible_text;
    return out;
}

} // namespace

// 1. 触发场景:yubo2 第 1760 行形态 —— "\n\n\n" 后跟裸 invoke,名字写成 Bash,
//    参数值两侧各一个换行。
// 期望行为:Recovered,可见正文只剩块前空白,参数去掉两侧换行,名字规范成 bash。
// 回归:旧实现 provider 返回 tool_calls=0,回合静默结束,Bash 从未执行。
TEST(TextToolCallRecoveryTest, RecoversBareInvokeWithNewlineWrappedParameter) {
    const std::string input = "\n\n\n" + bash_invoke("Bash", "ls -la");
    const auto result = recover_text_tool_calls(input, test_tools());

    ASSERT_EQ(result.diagnostic.outcome, Outcome::Recovered) << result.diagnostic.error;
    EXPECT_EQ(result.visible_text, "\n\n\n");
    ASSERT_EQ(result.tool_calls.size(), 1u);
    EXPECT_EQ(result.tool_calls[0].function_name, "bash");
    EXPECT_EQ(result.tool_calls[0].id.rfind("call_text_", 0), 0u);
    EXPECT_EQ(result.tool_calls[0].id.size(), std::string("call_text_").size() + 24);
    EXPECT_EQ(args_of(result.tool_calls[0]), nlohmann::json({{"command", "ls -la"}}));
    EXPECT_EQ(result.diagnostic.format, "invoke");
    EXPECT_EQ(result.diagnostic.recovered_count, 1);
}

// 2. 触发场景:第 1745 行形态 —— 两个连续的 <dots_function_call> 外壳块。
// 期望行为:2 个调用,id 互不相同,format=dots_function_call。
TEST(TextToolCallRecoveryTest, RecoversConsecutiveDotsWrappers) {
    const std::string input =
        "<dots_function_call>\n" + bash_invoke("bash", "Get-ChildItem A") +
        "\n</dots_function_call>\n<dots_function_call>\n" +
        bash_invoke("bash", "Get-ChildItem B") + "\n</dots_function_call>";
    const auto result = recover_text_tool_calls(input, test_tools());

    ASSERT_EQ(result.diagnostic.outcome, Outcome::Recovered) << result.diagnostic.error;
    ASSERT_EQ(result.tool_calls.size(), 2u);
    EXPECT_NE(result.tool_calls[0].id, result.tool_calls[1].id);
    EXPECT_EQ(args_of(result.tool_calls[0])["command"], "Get-ChildItem A");
    EXPECT_EQ(args_of(result.tool_calls[1])["command"], "Get-ChildItem B");
    EXPECT_EQ(result.diagnostic.format, "dots_function_call");
    EXPECT_EQ(result.visible_text, "");
}

// 3. 触发场景:一个 <function_calls> 外壳里有多个 invoke(Claude 风格模板)。
// 期望行为:按顺序恢复成多个调用。
TEST(TextToolCallRecoveryTest, RecoversFunctionCallsWrapperWithMultipleInvokes) {
    const std::string input =
        "<function_calls>\n" + bash_invoke("bash", "pwd") +
        "\n<invoke name=\"file_read\">\n<parameter name=\"file_path\">README.md"
        "</parameter>\n</invoke>\n</function_calls>\n";
    const auto result = recover_text_tool_calls(input, test_tools());

    ASSERT_EQ(result.diagnostic.outcome, Outcome::Recovered) << result.diagnostic.error;
    ASSERT_EQ(result.tool_calls.size(), 2u);
    EXPECT_EQ(result.tool_calls[0].function_name, "bash");
    EXPECT_EQ(result.tool_calls[1].function_name, "file_read");
    EXPECT_EQ(args_of(result.tool_calls[1])["file_path"], "README.md");
    EXPECT_EQ(result.diagnostic.format, "function_calls");
}

// 4. 触发场景:模型写 `Bash`;以及请求表里同时有 foo / Foo 时写 `FOO`。
// 期望行为:唯一候选时规范成请求表里的名字;有歧义时不猜,Rejected(unknown_tool)。
TEST(TextToolCallRecoveryTest, CaseInsensitiveToolNameMapsToRequestName) {
    const auto ok = recover_text_tool_calls(bash_invoke("BASH", "ls"), test_tools());
    ASSERT_EQ(ok.diagnostic.outcome, Outcome::Recovered) << ok.diagnostic.error;
    EXPECT_EQ(ok.tool_calls[0].function_name, "bash");

    const std::vector<ToolDef> ambiguous{
        make_tool("foo", nlohmann::json::object()),
        make_tool("Foo", nlohmann::json::object()),
    };
    const auto rejected =
        recover_text_tool_calls("<invoke name=\"FOO\">\n</invoke>", ambiguous);
    EXPECT_EQ(rejected.diagnostic.outcome, Outcome::Rejected);
    EXPECT_EQ(rejected.diagnostic.reason, "unknown_tool");
    EXPECT_TRUE(rejected.tool_calls.empty());
}

// 5. 触发场景:参数按 schema 转换类型。注意:这是自造 schema,不是现场形态
//    (现场写的是 schema 里没声明的 `timeout`,见用例 6)。
// 期望行为:integer / boolean / number / array / object 按 JSON 解析;
//    ["integer","null"] 接受 null;string 保持字符串(即使长得像数字);
//    ["string","integer"] 解析失败时退回字符串。
TEST(TextToolCallRecoveryTest, CoercesTypedParametersBySchema) {
    const std::vector<ToolDef> tools{make_tool("typed", {
        {"count", {{"type", "integer"}}},
        {"flag", {{"type", "boolean"}}},
        {"ratio", {{"type", "number"}}},
        {"limit", {{"type", nlohmann::json::array({"integer", "null"})}}},
        {"label", {{"type", "string"}}},
        {"items", {{"type", "array"}}},
        {"opts", {{"type", "object"}}},
        {"mixed", {{"type", nlohmann::json::array({"string", "integer"})}}},
        {"mixed_num", {{"type", nlohmann::json::array({"string", "integer"})}}},
    })};
    const std::string input =
        "<invoke name=\"typed\">\n"
        "<parameter name=\"count\">\n42\n</parameter>\n"
        "<parameter name=\"flag\">true</parameter>\n"
        "<parameter name=\"ratio\">0.5</parameter>\n"
        "<parameter name=\"limit\">null</parameter>\n"
        "<parameter name=\"label\">123</parameter>\n"
        "<parameter name=\"items\">[1, \"two\"]</parameter>\n"
        "<parameter name=\"opts\">{\"depth\": 2}</parameter>\n"
        "<parameter name=\"mixed\">abc</parameter>\n"
        "<parameter name=\"mixed_num\">7</parameter>\n"
        "</invoke>";
    const auto result = recover_text_tool_calls(input, tools);

    ASSERT_EQ(result.diagnostic.outcome, Outcome::Recovered) << result.diagnostic.error;
    const auto args = args_of(result.tool_calls[0]);
    EXPECT_EQ(args["count"], 42);
    EXPECT_EQ(args["flag"], true);
    EXPECT_DOUBLE_EQ(args["ratio"].get<double>(), 0.5);
    EXPECT_TRUE(args["limit"].is_null());
    EXPECT_EQ(args["label"], "123");
    EXPECT_EQ(args["items"], nlohmann::json::parse("[1, \"two\"]"));
    EXPECT_EQ(args["opts"], nlohmann::json({{"depth", 2}}));
    EXPECT_EQ(args["mixed"], "abc");
    EXPECT_EQ(args["mixed_num"], 7);
}

// 6. 触发场景:现场形态 —— 模型写了 schema 里不存在的 `timeout`(bash 的真实参数
//    叫 timeout_ms)。
// 期望行为:未声明参数原样作为字符串 "5000" 传入(由工具自己忽略或报错);
//    以 { 开头且能解析的未声明值按 JSON 传入。
TEST(TextToolCallRecoveryTest, UndeclaredParameterStaysString) {
    const std::string input =
        "<invoke name=\"bash\">\n"
        "<parameter name=\"command\">\nsleep 1\n</parameter>\n"
        "<parameter name=\"timeout\">\n5000\n</parameter>\n"
        "<parameter name=\"extra\">{\"a\": 1}</parameter>\n"
        "</invoke>";
    const auto result = recover_text_tool_calls(input, test_tools());

    ASSERT_EQ(result.diagnostic.outcome, Outcome::Recovered) << result.diagnostic.error;
    const auto args = args_of(result.tool_calls[0]);
    EXPECT_EQ(args["timeout"], "5000");
    EXPECT_EQ(args["extra"], nlohmann::json({{"a", 1}}));
    EXPECT_EQ(args["command"], "sleep 1");
}

// 7. 触发场景:integer 参数写成 "5s"。
// 期望行为:Rejected(bad_param),错误文案指出参数、工具、期望类型与实际值;不执行。
TEST(TextToolCallRecoveryTest, RejectsParameterOfWrongType) {
    const std::string input =
        "<invoke name=\"bash\">\n"
        "<parameter name=\"command\">ls</parameter>\n"
        "<parameter name=\"timeout_ms\">5s</parameter>\n"
        "</invoke>";
    const auto result = recover_text_tool_calls(input, test_tools());

    EXPECT_EQ(result.diagnostic.outcome, Outcome::Rejected);
    EXPECT_EQ(result.diagnostic.reason, "bad_param");
    EXPECT_EQ(result.diagnostic.error,
              "parameter \"timeout_ms\" of tool \"bash\" expects integer, got \"5s\"");
    EXPECT_TRUE(result.tool_calls.empty());
    EXPECT_EQ(result.visible_text, "");
}

// 8. 触发场景:第 1416 行形态 —— `<invoke name="exec">`,exec 根本不是 ACECode 的工具。
// 期望行为:Rejected(unknown_tool),标记不出现在可见正文里,attempted_tools 记原名。
// 回归:旧实现标记被当正文(或被 DSML 类过滤器藏起)后回合静默结束。
TEST(TextToolCallRecoveryTest, RejectsUnknownToolAndHidesMarkup) {
    const std::string input = "\n\n\n" + bash_invoke("exec", "git status");
    const auto result = recover_text_tool_calls(input, test_tools());

    EXPECT_EQ(result.diagnostic.outcome, Outcome::Rejected);
    EXPECT_EQ(result.diagnostic.reason, "unknown_tool");
    EXPECT_EQ(result.diagnostic.error, "tool \"exec\" is not available");
    EXPECT_EQ(result.visible_text, "\n\n\n");
    EXPECT_EQ(result.visible_text.find("<invoke"), std::string::npos);
    ASSERT_EQ(result.diagnostic.attempted_tools.size(), 1u);
    EXPECT_EQ(result.diagnostic.attempted_tools[0], "exec");
    EXPECT_NE(result.diagnostic.raw_excerpt.find("exec"), std::string::npos);
    EXPECT_TRUE(result.tool_calls.empty());
}

// 9. 触发场景:模型在 ``` 或 ~~~ 围栏里给出调用示例。
// 期望行为:围栏里的是示例,原样当正文,诊断为 None。
TEST(TextToolCallRecoveryTest, LeavesFencedExampleAsText) {
    for (const std::string fence : {"```", "~~~"}) {
        const std::string input =
            "Example:\n" + fence + "xml\n" + bash_invoke("bash", "ls") + "\n" + fence +
            "\n";
        const auto result = recover_text_tool_calls(input, test_tools());
        EXPECT_EQ(result.diagnostic.outcome, Outcome::None) << fence;
        EXPECT_EQ(result.visible_text, input) << fence;
        EXPECT_TRUE(result.tool_calls.empty()) << fence;
    }
}

// 10. 触发场景:正文行中提到标签(行内代码或普通行中)。
// 期望行为:执行级只认行首,原样当正文。
TEST(TextToolCallRecoveryTest, LeavesInlineMentionAsText) {
    const std::string input =
        "Use `<invoke name=\"bash\">` to call tools.\n"
        "The <tool_call> tag wraps JSON in Hermes templates.\n";
    const auto result = recover_text_tool_calls(input, test_tools());
    EXPECT_EQ(result.diagnostic.outcome, Outcome::None);
    EXPECT_EQ(result.visible_text, input);
}

// 11. 触发场景:一个完整块后面还跟着正文(模型在展示调用长什么样)。
// 期望行为:块后出现非空白正文 → 不是调用,整段原样当正文。
TEST(TextToolCallRecoveryTest, IllustrativeBlockFollowedByProseIsText) {
    const std::string input =
        bash_invoke("bash", "ls") + "\nThat is how a call looks.";
    const auto result = recover_text_tool_calls(input, test_tools());
    EXPECT_EQ(result.diagnostic.outcome, Outcome::None);
    EXPECT_EQ(result.visible_text, input);
    EXPECT_TRUE(result.tool_calls.empty());
}

// 12. 触发场景:块前有正文(例如转述文件 / 网页里的内容,或解释 XML)。
// 期望行为:语法完整也**不执行**,Rejected(prose_prefix);标记隐藏,块前正文保留。
//     这是安全边界:yolo 与 goal 无人值守下权限门会自动放行,「转述的内容被执行」
//     没有确认兜底,所以宁可多一次纠正请求。
TEST(TextToolCallRecoveryTest, ProsePrefixedBlockIsRejectedNotExecuted) {
    const std::string input =
        "Here is an example:\n" + bash_invoke("bash", "rm -rf build");
    const auto result = recover_text_tool_calls(input, test_tools());

    EXPECT_EQ(result.diagnostic.outcome, Outcome::Rejected);
    EXPECT_EQ(result.diagnostic.reason, "prose_prefix");
    EXPECT_TRUE(result.tool_calls.empty());
    EXPECT_EQ(result.visible_text, "Here is an example:\n");
    ASSERT_EQ(result.candidate_calls.size(), 1u);
    EXPECT_EQ(result.candidate_calls[0].function_name, "bash");
}

// 13. 触发场景:行首开标签后语法立即偏离(`<invoked …`),或 <tool_call> 里不是
//     JSON / function 形式。
// 期望行为:偏离的那一刻就把扣住的内容当正文放出,不拖到流结束;后续正文照常流出。
TEST(TextToolCallRecoveryTest, NonGrammaticalOpenerIsReleasedImmediately) {
    TextToolCallStreamFilter filter(test_tools());
    EXPECT_EQ(filter.push("<invoked the tool\n"), "<invoked the tool\n");
    EXPECT_FALSE(filter.capturing());

    EXPECT_EQ(filter.push("<tool_call>\nhello"), "<tool_call>\nhello");
    EXPECT_FALSE(filter.capturing());
    const auto result = filter.finish();
    EXPECT_EQ(result.diagnostic.outcome, Outcome::None);
    EXPECT_EQ(result.visible_text, "");
}

// 14. 触发场景:网关逐字节下发(每个 chunk 1 字节)。对照 DSML 的
//     HoldsMarkerAcrossEveryByteBoundary。
// 期望行为:任何切分点都不泄漏 `<`,最终恢复出同样的调用。
TEST(TextToolCallRecoveryTest, HoldsCandidateAcrossEveryByteBoundary) {
    const std::string input = "\n\n" + bash_invoke("Bash", "git status");
    TextToolCallStreamFilter filter(test_tools());
    std::string visible;
    for (std::size_t i = 0; i < input.size(); ++i) {
        visible += filter.push(std::string_view(input.data() + i, 1));
        EXPECT_EQ(visible.find('<'), std::string::npos) << "leaked at byte " << i;
    }
    const auto result = filter.finish();
    visible += result.visible_text;

    ASSERT_EQ(result.diagnostic.outcome, Outcome::Recovered) << result.diagnostic.error;
    EXPECT_EQ(visible, "\n\n");
    ASSERT_EQ(result.tool_calls.size(), 1u);
    EXPECT_EQ(args_of(result.tool_calls[0])["command"], "git status");

    // 同一输入按 7 字节切分,结果一致。
    const auto chunked = stream_in_chunks(input, test_tools(), 7);
    EXPECT_EQ(chunked.result.diagnostic.outcome, Outcome::Recovered);
    EXPECT_EQ(chunked.visible, "\n\n");
}

// 15. 触发场景:流在参数值中途结束(输出长度上限或网关断流)。
// 期望行为:Rejected(truncated),错误指出缺的结束标签;标记不外泄;记下已写出的工具名。
TEST(TextToolCallRecoveryTest, TruncatedCallIsRejected) {
    const std::string input =
        "\n<invoke name=\"bash\">\n<parameter name=\"command\">\nls -la";
    const auto result = recover_text_tool_calls(input, test_tools());

    EXPECT_EQ(result.diagnostic.outcome, Outcome::Rejected);
    EXPECT_EQ(result.diagnostic.reason, "truncated");
    EXPECT_EQ(result.diagnostic.error,
              "response ended inside a text tool call (missing </parameter>)");
    EXPECT_EQ(result.visible_text, "\n");
    ASSERT_EQ(result.diagnostic.attempted_tools.size(), 1u);
    EXPECT_EQ(result.diagnostic.attempted_tools[0], "bash");
    EXPECT_TRUE(result.tool_calls.empty());
}

// 16. 触发场景:<function_calls> 里的 invoke 全部闭合,只缺 </function_calls>
//     (网关把外壳闭合标签设成了 stop sequence),或外壳闭合标签只写了一半。
// 期望行为:按完整处理,Recovered。
TEST(TextToolCallRecoveryTest, MissingOuterCloseAtEofIsComplete) {
    const std::string body = "<function_calls>\n" + bash_invoke("bash", "ls") + "\n";
    const auto missing = recover_text_tool_calls(body, test_tools());
    EXPECT_EQ(missing.diagnostic.outcome, Outcome::Recovered) << missing.diagnostic.error;

    const auto half = recover_text_tool_calls(body + "</function_", test_tools());
    EXPECT_EQ(half.diagnostic.outcome, Outcome::Recovered) << half.diagnostic.error;
    EXPECT_EQ(half.visible_text, "");
}

// 17. 触发场景:块后跟聊天模板的特殊 token(`<|im_end|>`),或裸 invoke 后多一个
//     孤立的 `</function_calls>`。
// 期望行为:都属于 TRAILER,忽略后 Recovered;标记与 token 都不外泄。
TEST(TextToolCallRecoveryTest, TrailingSpecialTokensAndOrphanCloseAreIgnored) {
    const std::string hermes =
        "<tool_call>\n{\"name\": \"bash\", \"arguments\": {\"command\": \"ls\"}}\n"
        "</tool_call><|im_end|>";
    const auto a = recover_text_tool_calls(hermes, test_tools());
    EXPECT_EQ(a.diagnostic.outcome, Outcome::Recovered) << a.diagnostic.error;
    EXPECT_EQ(a.visible_text, "");

    const std::string orphan = bash_invoke("bash", "ls") + "\n</function_calls>\n";
    const auto b = recover_text_tool_calls(orphan, test_tools());
    EXPECT_EQ(b.diagnostic.outcome, Outcome::Recovered) << b.diagnostic.error;
    EXPECT_EQ(b.visible_text, "");
}

// 18. 触发场景:Hermes / Qwen2.5 模板的 JSON 形式,arguments 为对象或 JSON 字符串。
// 期望行为:两种写法都恢复成同样的参数,format=tool_call_json;
//     arguments 不是 object 时 Rejected(bad_param)。
TEST(TextToolCallRecoveryTest, RecoversHermesJsonToolCall) {
    const auto obj = recover_text_tool_calls(
        "<tool_call>\n{\"name\": \"bash\", \"arguments\": {\"command\": \"ls\", "
        "\"timeout_ms\": 1000}}\n</tool_call>",
        test_tools());
    ASSERT_EQ(obj.diagnostic.outcome, Outcome::Recovered) << obj.diagnostic.error;
    EXPECT_EQ(args_of(obj.tool_calls[0]),
              nlohmann::json({{"command", "ls"}, {"timeout_ms", 1000}}));
    EXPECT_EQ(obj.diagnostic.format, "tool_call_json");

    const auto str = recover_text_tool_calls(
        "<tool_call>\n{\"name\": \"bash\", \"arguments\": \"{\\\"command\\\": \\\"ls\\\"}\"}"
        "\n</tool_call>",
        test_tools());
    ASSERT_EQ(str.diagnostic.outcome, Outcome::Recovered) << str.diagnostic.error;
    EXPECT_EQ(args_of(str.tool_calls[0]), nlohmann::json({{"command", "ls"}}));

    const auto bad = recover_text_tool_calls(
        "<tool_call>\n{\"name\": \"bash\", \"arguments\": [1]}\n</tool_call>",
        test_tools());
    EXPECT_EQ(bad.diagnostic.outcome, Outcome::Rejected);
    EXPECT_EQ(bad.diagnostic.reason, "bad_param");
}

// 19. 触发场景:Qwen3-Coder 模板的 `<tool_call><function=…><parameter=…>` 形式。
// 期望行为:Recovered,format=tool_call_function,参数值同样去掉两侧换行。
TEST(TextToolCallRecoveryTest, RecoversQwenCoderFunctionForm) {
    const std::string input =
        "<tool_call>\n<function=bash>\n<parameter=command>\nls -la\n</parameter>\n"
        "<parameter=timeout_ms>\n3000\n</parameter>\n</function>\n</tool_call>";
    const auto result = recover_text_tool_calls(input, test_tools());

    ASSERT_EQ(result.diagnostic.outcome, Outcome::Recovered) << result.diagnostic.error;
    EXPECT_EQ(result.diagnostic.format, "tool_call_function");
    EXPECT_EQ(args_of(result.tool_calls[0]),
              nlohmann::json({{"command", "ls -la"}, {"timeout_ms", 3000}}));
}

// 20. 触发场景:同名参数写了两次。
// 期望行为:不猜哪个算数,Rejected(bad_param)。
TEST(TextToolCallRecoveryTest, RejectsDuplicateParameterName) {
    const std::string input =
        "<invoke name=\"bash\">\n"
        "<parameter name=\"command\">ls</parameter>\n"
        "<parameter name=\"command\">rm -rf /</parameter>\n"
        "</invoke>";
    const auto result = recover_text_tool_calls(input, test_tools());
    EXPECT_EQ(result.diagnostic.outcome, Outcome::Rejected);
    EXPECT_EQ(result.diagnostic.reason, "bad_param");
    EXPECT_TRUE(result.tool_calls.empty());
}

// 21. 触发场景:「工具重写」把 file_read 映射成 read,请求表里是模型侧名 read;
//     模型写原生名 file_read、模型侧名 read 或大小写不同的 Read。
// 期望行为:三种写法都规范成请求表里的 read(与 DSML add_allowed_tool_name 同一口径)。
TEST(TextToolCallRecoveryTest, AcceptsNativeAndModelFacingNames) {
    acecode::ScopedModelToolNameMappings scoped{{"file_read", "read"}};
    const std::vector<ToolDef> tools{make_tool("read", {
        {"file_path", {{"type", "string"}}},
    })};
    for (const std::string written : {"file_read", "read", "Read"}) {
        const auto result = recover_text_tool_calls(
            "<invoke name=\"" + written +
                "\">\n<parameter name=\"file_path\">a.txt</parameter>\n</invoke>",
            tools);
        ASSERT_EQ(result.diagnostic.outcome, Outcome::Recovered)
            << written << ": " << result.diagnostic.error;
        EXPECT_EQ(result.tool_calls[0].function_name, "read") << written;
    }
}

// 22. 触发场景:provider 重试(上一次尝试停在扣住态)后 reset()。
// 期望行为:上一次的候选被丢弃,不会与新尝试的正文拼接,也不会在 finish 时报诊断。
TEST(TextToolCallRecoveryTest, ResetDropsCandidateFromPreviousAttempt) {
    TextToolCallStreamFilter filter(test_tools());
    EXPECT_EQ(filter.push("<invoke name=\"bash\">\n<param"), "");
    EXPECT_TRUE(filter.capturing());
    EXPECT_GT(filter.held_bytes(), 0u);

    filter.reset();
    EXPECT_FALSE(filter.capturing());
    EXPECT_EQ(filter.held_bytes(), 0u);
    EXPECT_EQ(filter.push("hello"), "hello");
    const auto result = filter.finish();
    EXPECT_EQ(result.diagnostic.outcome, Outcome::None);
    EXPECT_EQ(result.visible_text, "");
}

// 23. 触发场景:文本形式的 file_write,参数值 1MB,网关按 16 字节一块下发。
// 期望行为:扣住期间只在新数据上找结束标签,总扫描字节数 <= 2 × 输入长度。
//     用扫描计数代替计时,避免测试不稳定;旧方案每个 chunk 都从头重解析,这里
//     会是 O(n²)(约 3.3 万次 × 平均 0.5MB)。
TEST(TextToolCallRecoveryTest, HeldScanIsLinearInInputSize) {
    const std::vector<ToolDef> tools{make_tool("file_write", {
        {"file_path", {{"type", "string"}}},
        {"content", {{"type", "string"}}},
    })};
    const std::string payload(1024 * 1024, 'x');
    const std::string input =
        "<invoke name=\"file_write\">\n<parameter name=\"file_path\">big.txt"
        "</parameter>\n<parameter name=\"content\">" +
        payload + "</parameter>\n</invoke>";

    TextToolCallStreamFilter filter(tools);
    std::string visible;
    for (std::size_t i = 0; i < input.size(); i += 16) {
        visible += filter.push(std::string_view(input).substr(i, 16));
    }
    const auto result = filter.finish();

    ASSERT_EQ(result.diagnostic.outcome, Outcome::Recovered) << result.diagnostic.error;
    EXPECT_EQ(args_of(result.tool_calls[0])["content"].get<std::string>().size(),
              payload.size());
    EXPECT_LE(filter.debug_bytes_scanned(), 2 * input.size());
    EXPECT_GE(filter.debug_bytes_scanned(), payload.size());
    EXPECT_EQ(visible, "");
}

// 24. 触发场景:执行级没认出,但正文里明显在尝试调用工具 —— 行中的
//     `<invoke name=`、行首未闭合的 `<parameter name=`、行首 `<tool_call>` 后
//     JSON 写坏。
// 期望行为:命中可疑级(Rejected/malformed),visible_cut 指向命中行行首;
//     围栏内、行内代码里、普通比较符号不命中。
TEST(TextToolCallRecoveryTest, SuspiciousDetectorFlagsMidLineAndMalformed) {
    const std::string first_line = u8"好的,我来看看。\n";
    const auto mid = acecode::detect_suspicious_text_tool_call(
        first_line + "Let me run <invoke name=\"bash\">");
    ASSERT_TRUE(mid.has_value());
    EXPECT_EQ(mid->outcome, Outcome::Rejected);
    EXPECT_EQ(mid->reason, "malformed");
    EXPECT_EQ(mid->format, "invoke");
    EXPECT_EQ(mid->visible_cut, first_line.size());

    const auto param = acecode::detect_suspicious_text_tool_call(
        "Intro\n<parameter name=\"command\">ls");
    ASSERT_TRUE(param.has_value());
    EXPECT_EQ(param->visible_cut, 6u);

    const auto broken_json =
        acecode::detect_suspicious_text_tool_call("<tool_call>\n{broken");
    ASSERT_TRUE(broken_json.has_value());
    EXPECT_EQ(broken_json->format, "tool_call_json");
    EXPECT_EQ(broken_json->visible_cut, 0u);

    EXPECT_FALSE(acecode::detect_suspicious_text_tool_call(
                     "```\n<invoke name=\"bash\">\n```\n")
                     .has_value());
    EXPECT_FALSE(acecode::detect_suspicious_text_tool_call(
                     "Write `<invoke name=\"bash\">` to call.")
                     .has_value());
    EXPECT_FALSE(acecode::detect_suspicious_text_tool_call("if a < b and c > d")
                     .has_value());
}

// 25. 触发场景:压缩摘要校验。第 1002 行形态的摘要只剩 `<dots_function_call>…`;
//     块后面跟着 next steps 正文的摘要同样被污染。
// 期望行为:任意位置出现(行首、围栏外)调用标记即 true;DSML 标记也算;
//     围栏里的示例与普通散文为 false。
// 回归:旧实现把这种回复原样当摘要落盘,压缩后历史里再也没有原生调用可参照。
TEST(TextToolCallRecoveryTest, MarkupDetectorFlagsSummaryPollutionAnywhere) {
    const std::string polluted =
        "<dots_function_call>\n" + bash_invoke("bash", "git status") +
        "\n</dots_function_call>";
    EXPECT_TRUE(acecode::text_contains_tool_call_markup(polluted));
    EXPECT_TRUE(acecode::text_contains_tool_call_markup(
        "Progress so far.\n" + polluted + "\nNext steps: run the tests."));
    EXPECT_FALSE(acecode::text_contains_tool_call_markup(
        "Summary:\n```xml\n" + polluted + "\n```\nDone."));
    EXPECT_TRUE(acecode::text_contains_tool_call_markup(
        u8"<｜DSML｜tool_calls>\n<｜DSML｜invoke name=\"bash\">"));
    EXPECT_FALSE(acecode::text_contains_tool_call_markup(
        u8"尚无进展。用户要求修复 a < b 的比较。"));
}

// 26. 触发场景:第 1758 行形态的混合回复 —— 既有原生调用又有正文文本调用。
//     Qwen/Hermes 类模板常把原生调用在正文里再回显一遍。
// 期望行为:与原生调用完全一致(名字规范化 + 参数 JSON 相等)的算回显,剔除后
//     诊断为 None;参数不同的保留,诊断 IgnoredWithNative,说明里只列工具名与参数键名。
// 回归:不做比对就会提示模型「这些没执行」,诱导它把同一个调用重发一次。
TEST(TextToolCallRecoveryTest, EchoOfNativeCallIsDropped) {
    const auto tools = test_tools();
    const std::vector<ToolCall> native{{"call_1", "bash", "{\"command\":\"ls\"}"}};

    const auto echo = recover_text_tool_calls(bash_invoke("Bash", "ls"), tools);
    auto echo_calls = echo.candidate_calls;
    acecode::drop_echoes_of_native_calls(echo_calls, native, tools);
    EXPECT_TRUE(echo_calls.empty());
    EXPECT_EQ(acecode::diagnose_text_tool_calls_with_native(echo, native, tools).outcome,
              Outcome::None);

    const auto different =
        recover_text_tool_calls(bash_invoke("Bash", "Get-ChildItem src"), tools);
    const auto diag =
        acecode::diagnose_text_tool_calls_with_native(different, native, tools);
    EXPECT_EQ(diag.outcome, Outcome::IgnoredWithNative);
    ASSERT_EQ(diag.unexecuted_detail.size(), 1u);
    EXPECT_EQ(diag.unexecuted_detail[0], "bash(command)");
    const std::string note = acecode::build_text_tool_call_ignored_note(diag);
    EXPECT_NE(note.find("bash(command)"), std::string::npos);
    EXPECT_NE(note.find("NOT executed"), std::string::npos);
    EXPECT_EQ(note.find("Get-ChildItem"), std::string::npos);
    EXPECT_EQ(acecode::build_text_tool_call_ignored_note(TextToolCallDiagnostic{}), "");
}

// 27. 触发场景:构造纠正提示。
// 期望行为:unknown_tool 时列出本次请求的模型侧工具名;truncated_by_length 时点出
//     输出长度上限并建议拆小;文案不含 `<invoke` 等可模仿的字面标签。
TEST(TextToolCallRecoveryTest, CorrectionPromptUsesModelFacingNamesAndMentionsLengthLimit) {
    TextToolCallDiagnostic unknown;
    unknown.outcome = Outcome::Rejected;
    unknown.reason = "unknown_tool";
    unknown.error = "tool \"exec\" is not available";
    const std::string prompt =
        acecode::build_text_tool_call_correction_prompt(unknown, {"read", "bash"});
    EXPECT_EQ(prompt.rfind("[SYSTEM NOTE]", 0), 0u);
    EXPECT_NE(prompt.find("Problem: tool \"exec\" is not available."), std::string::npos);
    EXPECT_NE(prompt.find("Available tools: read, bash."), std::string::npos);
    EXPECT_NE(prompt.find("native tool-calling interface"), std::string::npos);
    EXPECT_EQ(prompt.find("<invoke"), std::string::npos);
    EXPECT_EQ(prompt.find("output length limit"), std::string::npos);

    TextToolCallDiagnostic truncated;
    truncated.outcome = Outcome::Rejected;
    truncated.reason = "truncated_by_length";
    truncated.error = "response ended inside a text tool call (missing </parameter>)";
    const std::string length_prompt =
        acecode::build_text_tool_call_correction_prompt(truncated, {"bash"});
    EXPECT_NE(length_prompt.find("output length limit"), std::string::npos);
    EXPECT_EQ(length_prompt.find("Available tools:"), std::string::npos);
}

// 28. 触发场景:同一段回复被网关按不同大小切块下发。
// 期望行为:增量解析的结论(outcome / 可见正文 / 调用参数)与一次性解析完全一致 ——
//     扣住、释放、重新扫描的路径不因切分点不同而改变结果。
TEST(TextToolCallRecoveryTest, ChunkingDoesNotChangeOutcome) {
    const std::vector<std::string> inputs{
        "\n\n\n" + bash_invoke("Bash", "ls -la"),
        "<function_calls>\n" + bash_invoke("bash", "pwd") +
            "\n</function_calls><|im_end|>",
        "<tool_call>\n{\"name\": \"bash\", \"arguments\": {\"command\": \"ls\"}}\n"
        "</tool_call>",
        "<tool_call>\n<function=bash>\n<parameter=command>\nls\n</parameter>\n"
        "</function>\n</tool_call>",
        "Here is an example:\n" + bash_invoke("bash", "rm -rf build"),
        bash_invoke("bash", "ls") + "\nThat is how a call looks.\n" +
            bash_invoke("bash", "pwd"),
        "<invoked\n<tool_call>\nhello\n<invoke name=\"exec\">\n</invoke>",
        "```xml\n" + bash_invoke("bash", "ls") + "\n```\n",
        "\n<invoke name=\"bash\">\n<parameter name=\"command\">\nls </param",
    };
    for (const auto& input : inputs) {
        const auto whole = recover_text_tool_calls(input, test_tools());
        for (std::size_t chunk : {1u, 2u, 3u, 5u, 11u, 16u}) {
            const auto streamed = stream_in_chunks(input, test_tools(), chunk);
            EXPECT_EQ(streamed.result.diagnostic.outcome, whole.diagnostic.outcome)
                << "chunk=" << chunk << " input=" << input;
            EXPECT_EQ(streamed.result.diagnostic.reason, whole.diagnostic.reason)
                << "chunk=" << chunk << " input=" << input;
            EXPECT_EQ(streamed.visible, whole.visible_text)
                << "chunk=" << chunk << " input=" << input;
            ASSERT_EQ(streamed.result.tool_calls.size(), whole.tool_calls.size());
            for (std::size_t i = 0; i < whole.tool_calls.size(); ++i) {
                EXPECT_EQ(streamed.result.tool_calls[i].function_arguments,
                          whole.tool_calls[i].function_arguments);
            }
        }
    }
}

// ---- 历史清洗(Commit F) --------------------------------------------------

namespace {

using acecode::ChatMessage;
using acecode::kTextToolCallHistoryPlaceholder;
using acecode::sanitize_text_tool_call_history;

const std::string kTestSummaryPrefix = "[Conversation summary]";

ChatMessage history_msg(std::string role, std::string content) {
    ChatMessage m;
    m.role = std::move(role);
    m.content = std::move(content);
    return m;
}

std::string legacy_bash_call() {
    return "\n\n\n<invoke name=\"Bash\">\n<parameter name=\"command\">\n"
           "Get-ChildItem src\n</parameter>\n</invoke>";
}

} // namespace

// 场景:修复上线前落盘的老会话里,assistant 消息整条就是一段文本调用(yubo2 现场
// 第 1760 行的形态,也包括带 <dots_function_call> 外壳的、以及写到一半被截断的)。
// 期望:发给模型前换成固定说明,不再保留可模仿的调用样本。
// 回归表现:升级后老会话在下一次压缩前,模型继续照着历史里的 11 条样本写文本调用。
TEST(TextToolCallRecoveryTest, SanitizeReplacesPureTextCallAssistant) {
    std::vector<ChatMessage> history{
        history_msg("assistant", legacy_bash_call()),
        history_msg("assistant",
                    "\n<dots_function_call>\n<invoke name=\"Bash\">\n"
                    "<parameter name=\"command\">\nls\n</parameter>\n</invoke>\n"
                    "</dots_function_call>"),
        history_msg("assistant",
                    "\n<invoke name=\"Bash\">\n<parameter name=\"command\">\nls"),
    };
    sanitize_text_tool_call_history(history, kTestSummaryPrefix);
    for (const auto& m : history) {
        EXPECT_EQ(m.content, kTextToolCallHistoryPlaceholder);
    }
}

// 场景:用户自己粘贴的标记、块前有正文的解释性回复、带原生 tool_calls 的消息、
// 代码围栏里的示例。期望:一律原样保留 —— 它们是数据或解释,不是模仿样本。
TEST(TextToolCallRecoveryTest, SanitizeKeepsUserPastedMarkupAndProse) {
    ChatMessage with_native = history_msg("assistant", legacy_bash_call());
    with_native.tool_calls = nlohmann::json::array(
        {{{"id", "c1"}, {"type", "function"},
          {"function", {{"name", "bash"}, {"arguments", "{}"}}}}});
    std::vector<ChatMessage> history{
        history_msg("user", legacy_bash_call()),
        history_msg("assistant", "Here is an example:\n" + legacy_bash_call()),
        with_native,
        history_msg("assistant", "```xml\n" + legacy_bash_call() + "\n```"),
        history_msg("assistant", "plain answer without markup"),
    };
    const auto before = history;
    sanitize_text_tool_call_history(history, kTestSummaryPrefix);
    ASSERT_EQ(history.size(), before.size());
    for (std::size_t i = 0; i < history.size(); ++i) {
        EXPECT_EQ(history[i].content, before[i].content) << "message " << i;
    }
}

// 场景:压缩摘要被工具调用文本污染(yubo2 第 1002 行:摘要正文只剩
// <dots_function_call>…;以及摘要正文后面跟着一段调用)。期望:只剩调用块时
// 换成固定说明 + "(summary unavailable)";块前有正文时保留正文、末尾换成固定
// 说明;只打 is_compact_summary 标记、没有前缀的摘要同样处理。
TEST(TextToolCallRecoveryTest, SanitizeScrubsPollutedSummary) {
    const std::string head = kTestSummaryPrefix + "\n";
    ChatMessage flagged = history_msg("user", legacy_bash_call());
    flagged.is_compact_summary = true;
    std::vector<ChatMessage> history{
        history_msg("user", head +
                    "<dots_function_call>\n<invoke name=\"Bash\">\n"
                    "<parameter name=\"command\">\nls\n</parameter>\n</invoke>\n"
                    "</dots_function_call>"),
        history_msg("user", head + "Done so far: listed src.\n" + legacy_bash_call()),
        flagged,
    };
    sanitize_text_tool_call_history(history, kTestSummaryPrefix);
    EXPECT_EQ(history[0].content, head + kTextToolCallHistoryPlaceholder +
                                      std::string("\n(summary unavailable)"));
    EXPECT_EQ(history[1].content, head + "Done so far: listed src.\n\n" +
                                      kTextToolCallHistoryPlaceholder);
    EXPECT_EQ(history[2].content, std::string(kTextToolCallHistoryPlaceholder) +
                                      "\n(summary unavailable)");
}

// 场景:清洗在每次请求时都会跑一遍(model_facing_provider_messages)。期望:
// 幂等、只由内容决定 —— 再跑一次结果逐字节相同,不打穿 prompt cache 前缀。
TEST(TextToolCallRecoveryTest, SanitizeIsIdempotentAndByteStable) {
    std::vector<ChatMessage> history{
        history_msg("assistant", legacy_bash_call()),
        history_msg("user", kTestSummaryPrefix + "\nnotes\n" + legacy_bash_call()),
        history_msg("user", "keep me"),
    };
    sanitize_text_tool_call_history(history, kTestSummaryPrefix);
    const auto once = history;
    sanitize_text_tool_call_history(history, kTestSummaryPrefix);
    ASSERT_EQ(history.size(), once.size());
    for (std::size_t i = 0; i < history.size(); ++i) {
        EXPECT_EQ(history[i].content, once[i].content) << "message " << i;
    }
    EXPECT_EQ(history[2].content, "keep me");
}
