// 覆盖 src/adapters/tool/task_complete_tool.cpp 的核心行为(仿 Claude 桌面版 wrap_up
// 的「下一步」框):
// - 合法非空 summary(或别名 recap)→ success=true,回执写给模型,ToolSummary /
//   metadata.wrap_up 填充
// - 缺参 / 空串 / 纯空白 / 类型错误 / JSON 解析失败 → success=false + 明确错误提示
// - ask + 有效的第一个按钮 → 框显示问题与按钮;否则只留 recap,回执给出下一次的建议
// - 按钮以 / 或 ! 开头、含零宽字符等看不见的字符时被丢弃;推荐项被丢弃时问题一并不显示
// - 工具默认 is_read_only=true,保证 PermissionManager 放行
//
// 纯函数级测试,不依赖 TuiState / FTXUI / 线程,放 tests/tool/ 目录统一 glob。

#include <gtest/gtest.h>

#include "tool/task_complete_tool.hpp"
#include "tool/tool_executor.hpp"

#include <nlohmann/json.hpp>

using acecode::create_task_complete_tool;
using acecode::parse_task_complete_call;
using acecode::ToolContext;
using acecode::ToolImpl;
using acecode::ToolResult;

namespace {

// 小辅助:直接跑 tool,不带 context(task_complete 不使用 stream / abort)
ToolResult run(const nlohmann::json& args) {
    ToolImpl impl = create_task_complete_tool();
    ToolContext ctx;
    return impl.execute(args.dump(), ctx);
}

ToolResult run_raw(const std::string& args_json) {
    ToolImpl impl = create_task_complete_tool();
    ToolContext ctx;
    return impl.execute(args_json, ctx);
}

bool has_metric(const acecode::ToolSummary& s, const std::string& k,
                std::string* out_value = nullptr) {
    for (const auto& kv : s.metrics) {
        if (kv.first == k) {
            if (out_value) *out_value = kv.second;
            return true;
        }
    }
    return false;
}

bool advice_mentions(const acecode::TaskCompleteCall& call, const std::string& needle) {
    for (const auto& line : call.advice) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

// 场景:只有 summary(没有待用户决定的事)。
// 期望:成功;总结进 summary 指标与 metadata.wrap_up.recap;不画「下一步」框;
// 回执告诉模型这次没有框,而不是把 summary 原样回显。
TEST(TaskCompleteTool, ValidSummarySucceeds) {
    ToolResult r = run({{"summary", "Refactored auth module and added 4 tests"}});

    EXPECT_TRUE(r.success);
    EXPECT_NE(r.output.find("Nothing is pending"), std::string::npos);
    ASSERT_TRUE(r.summary.has_value());
    EXPECT_EQ(r.summary->verb, "complete");
    EXPECT_EQ(r.summary->object, "task");
    EXPECT_FALSE(r.summary->icon.empty());
    std::string stored_summary;
    EXPECT_TRUE(has_metric(*r.summary, "summary", &stored_summary));
    EXPECT_EQ(stored_summary, "Refactored auth module and added 4 tests");
    EXPECT_FALSE(has_metric(*r.summary, "ask"));

    ASSERT_TRUE(r.metadata.contains("wrap_up"));
    const auto& wrap_up = r.metadata["wrap_up"];
    EXPECT_EQ(wrap_up["recap"], "Refactored auth module and added 4 tests");
    EXPECT_FALSE(wrap_up.contains("box"));
    EXPECT_FALSE(wrap_up.contains("ask"));
    EXPECT_FALSE(wrap_up.contains("actions"));
}

// 场景:模型照 Claude 的字段名写 recap 而不是 summary。
// 期望:当作 summary 接受(别名),否则这次终止信号会失败、回合停不下来。
TEST(TaskCompleteTool, RecapAliasIsAccepted) {
    ToolResult r = run({{"recap", "改好了登录页,还没提交。"}});
    ASSERT_TRUE(r.success);
    std::string stored_summary;
    EXPECT_TRUE(has_metric(*r.summary, "summary", &stored_summary));
    EXPECT_EQ(stored_summary, "改好了登录页,还没提交。");
}

// 场景:缺 summary 字段 → success=false + 明确错误信息
TEST(TaskCompleteTool, MissingSummaryFails) {
    ToolResult r = run(nlohmann::json::object());
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.output.find("non-empty summary"), std::string::npos);
    EXPECT_FALSE(r.summary.has_value());
}

// 场景:空字符串 summary → 同样视作缺参
TEST(TaskCompleteTool, EmptySummaryFails) {
    ToolResult r = run({{"summary", ""}});
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.output.find("non-empty summary"), std::string::npos);
}

// 场景:只有空白字符(空格 / 换行 / tab)→ 也判定为空,防止模型拿"   "糊弄
TEST(TaskCompleteTool, WhitespaceOnlySummaryFails) {
    ToolResult r = run({{"summary", "   \n\t  "}});
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.output.find("non-empty summary"), std::string::npos);
}

// 场景:summary 字段类型错误(给了数字)→ 按缺参处理,不崩
TEST(TaskCompleteTool, WrongTypeSummaryFails) {
    ToolResult r = run({{"summary", 42}});
    EXPECT_FALSE(r.success);
}

// 场景:完全无效的 JSON → catch 后走缺参分支,绝不抛异常
TEST(TaskCompleteTool, InvalidJsonFailsGracefully) {
    ToolResult r = run_raw("not a json {{{{");
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.output.find("non-empty summary"), std::string::npos);
}

// 场景:summary 前后带空白但中间有内容 → 接受(模型输出经常带换行),存的是去掉首尾空白的文本
TEST(TaskCompleteTool, SummaryWithSurroundingWhitespaceSucceeds) {
    ToolResult r = run({{"summary", "  Done.  \n"}});
    ASSERT_TRUE(r.success);
    std::string stored_summary;
    EXPECT_TRUE(has_metric(*r.summary, "summary", &stored_summary));
    EXPECT_EQ(stored_summary, "Done.");
}

// 场景:回合结束时轮到用户决定(问题 + 推荐按钮 + 备选按钮)。
// 期望:框显示;metadata 带规范化后的问题与按钮,顺序保持(第一个是推荐项);
// summary 指标之外多一个 ask 指标,让没有框的 TUI 也能把问题显示出来。
TEST(TaskCompleteTool, AskWithActionsShowsBox) {
    ToolResult r = run({
        {"summary", "修好了空指针,测试全过,还没提交。"},
        {"ask", "现在提交吗?"},
        {"actions", nlohmann::json::array({{{"label", "提交"}}, {{"label", "先不提交"}}})},
    });
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.output, "Shown in the next-step box.");
    std::string ask_metric;
    EXPECT_TRUE(has_metric(*r.summary, "ask", &ask_metric));
    EXPECT_EQ(ask_metric, "现在提交吗?");

    const auto& wrap_up = r.metadata["wrap_up"];
    EXPECT_EQ(wrap_up["box"], true);
    EXPECT_EQ(wrap_up["ask"], "现在提交吗?");
    ASSERT_EQ(wrap_up["actions"].size(), 2u);
    EXPECT_EQ(wrap_up["actions"][0]["label"], "提交");
    EXPECT_EQ(wrap_up["actions"][1]["label"], "先不提交");
}

// 场景:模型直接把按钮写成字符串数组(不是 {"label": ...})。
// 期望:同样接受,按钮文字里的换行折成空格。
TEST(TaskCompleteTool, PlainStringActionsAreAccepted) {
    const auto call = parse_task_complete_call(nlohmann::json{
        {"summary", "Done."}, {"ask", "Push now?"},
        {"actions", nlohmann::json::array({"Push", "Not\nnow"})}}.dump());
    ASSERT_TRUE(call.box_shown);
    ASSERT_EQ(call.actions.size(), 2u);
    EXPECT_EQ(call.actions[1], "Not now");
}

// 场景:给了问题却没有按钮(或给了按钮却没有问题)。
// 期望:不画框、不保留问题(按钮也没了,用户无从点选);回执建议下一次要么补齐
// ask + actions,要么只写 summary。
TEST(TaskCompleteTool, AskWithoutActionsDrawsNoBox) {
    const auto call = parse_task_complete_call(nlohmann::json{
        {"summary", "Done."}, {"ask", "Ship it?"}}.dump());
    EXPECT_FALSE(call.box_shown);
    EXPECT_TRUE(call.ask.empty());
    EXPECT_TRUE(advice_mentions(call, "The box drew no buttons"));

    ToolResult r = run({{"summary", "Done."}, {"ask", "Ship it?"}});
    ASSERT_TRUE(r.success);
    EXPECT_NE(r.output.find("No box is drawn"), std::string::npos);
    EXPECT_NE(r.output.find("for your next call"), std::string::npos);
    EXPECT_FALSE(r.metadata["wrap_up"].contains("ask"));
    EXPECT_FALSE(has_metric(*r.summary, "ask"));

    const auto actions_only = parse_task_complete_call(nlohmann::json{
        {"summary", "Done."}, {"actions", nlohmann::json::array({{{"label", "Yes"}}})}}.dump());
    EXPECT_FALSE(actions_only.box_shown);
    EXPECT_TRUE(advice_mentions(actions_only, "The box drew no buttons"));
}

// 场景:推荐按钮以 "/" 开头(点下去会被当成斜杠命令执行)。
// 期望:推荐项被丢弃时其余按钮与问题一起丢弃,框不显示;blocked 时问题保留、
// 框照常显示(用户在输入框里回答)。
// 回归:不过滤时点按钮会把 "/commit" 当命令发出去,而不是作为回答。
TEST(TaskCompleteTool, DroppedRecommendationRemovesQuestion) {
    const nlohmann::json actions = nlohmann::json::array({{{"label", "/commit"}}, {{"label", "Later"}}});
    const auto call = parse_task_complete_call(nlohmann::json{
        {"summary", "Done."}, {"ask", "Commit?"}, {"actions", actions}}.dump());
    EXPECT_FALSE(call.box_shown);
    EXPECT_TRUE(call.actions.empty());
    EXPECT_TRUE(call.ask.empty());
    EXPECT_TRUE(advice_mentions(call, "starts with \"/\" or \"!\""));

    const auto blocked = parse_task_complete_call(nlohmann::json{
        {"summary", "Done."}, {"ask", "Commit?"}, {"actions", actions}, {"blocked", true}}.dump());
    EXPECT_TRUE(blocked.box_shown);
    EXPECT_EQ(blocked.ask, "Commit?");
    EXPECT_TRUE(blocked.actions.empty());
}

// 场景:备选按钮含零宽连接符(U+200D,常见于组合 emoji)。
// 期望:只丢这个备选按钮,推荐项与问题照常显示。
TEST(TaskCompleteTool, InvisibleCharacterDropsOnlyThatReply) {
    const auto call = parse_task_complete_call(nlohmann::json{
        {"summary", "Done."}, {"ask", "Fix it?"},
        {"actions", nlohmann::json::array({"Fix it", "No\xE2\x80\x8Dthanks"})}}.dump());
    ASSERT_TRUE(call.box_shown);
    ASSERT_EQ(call.actions.size(), 1u);
    EXPECT_EQ(call.actions[0], "Fix it");
    EXPECT_TRUE(advice_mentions(call, "invisible character"));
}

// 场景:给了三个按钮。
// 期望:只画前两个;回执提示按钮最多两个。
TEST(TaskCompleteTool, OnlyFirstTwoActionsAreShown) {
    const auto call = parse_task_complete_call(nlohmann::json{
        {"summary", "Done."}, {"ask", "Which one?"},
        {"actions", nlohmann::json::array({"A", "B", "C"})}}.dump());
    ASSERT_EQ(call.actions.size(), 2u);
    EXPECT_EQ(call.actions[0], "A");
    EXPECT_EQ(call.actions[1], "B");
    EXPECT_TRUE(advice_mentions(call, "this call had 3"));
}

// 场景:按钮文字写成了一句话(超过 24 个字符 / 4 个词)。
// 期望:按钮按 24 个字符截断(按钮放不下),回执点名这条文字太长;
// 中文按码点计数,截断不会切坏 UTF-8。
TEST(TaskCompleteTool, LongLabelIsClippedAndReported) {
    const std::string long_label = "Yes please go ahead and fix everything now";
    const auto call = parse_task_complete_call(nlohmann::json{
        {"summary", "Done."}, {"ask", "Fix?"},
        {"actions", nlohmann::json::array({long_label})}}.dump());
    ASSERT_EQ(call.actions.size(), 1u);
    EXPECT_EQ(call.actions[0], long_label.substr(0, 24));
    EXPECT_TRUE(advice_mentions(call, long_label));

    std::string chinese;
    for (int i = 0; i < 30; ++i) chinese += "改";
    const auto cjk = parse_task_complete_call(nlohmann::json{
        {"summary", "Done."}, {"ask", "Fix?"},
        {"actions", nlohmann::json::array({chinese})}}.dump());
    ASSERT_EQ(cjk.actions.size(), 1u);
    EXPECT_EQ(cjk.actions[0].size(), 24u * 3u);  // 每个汉字 3 字节,24 个码点
}

// 场景:summary 写成了长篇总结(超过约 300 字符)、问题超过 100 字符。
// 期望:内容不截断(旧会话的长总结照常显示),只在回执里提醒下一次写短。
TEST(TaskCompleteTool, LongSummaryAndAskAreReportedNotTruncated) {
    const std::string summary(320, 'x');
    const std::string ask = std::string(120, 'y') + "?";
    const auto call = parse_task_complete_call(nlohmann::json{
        {"summary", summary}, {"ask", ask},
        {"actions", nlohmann::json::array({"Yes"})}}.dump());
    EXPECT_EQ(call.summary, summary);
    EXPECT_EQ(call.ask, ask);
    EXPECT_TRUE(advice_mentions(call, "this one ran 320"));
    EXPECT_TRUE(advice_mentions(call, "this one ran 121"));
}

// 场景:工作停在这里等用户回答(blocked),但没给按钮。
// 期望:框照常显示问题(用户在输入框里回答),metadata 标 blocked。
TEST(TaskCompleteTool, BlockedShowsBoxWithoutButtons) {
    ToolResult r = run({{"summary", "缺少数据库密码,迁移停在第 2 步。"},
                        {"ask", "数据库密码是什么?"}, {"blocked", true}});
    ASSERT_TRUE(r.success);
    const auto& wrap_up = r.metadata["wrap_up"];
    EXPECT_EQ(wrap_up["box"], true);
    EXPECT_EQ(wrap_up["blocked"], true);
    EXPECT_EQ(wrap_up["ask"], "数据库密码是什么?");
    EXPECT_FALSE(wrap_up.contains("actions"));
}

// 场景:ToolImpl 默认 is_read_only=true —— 让 PermissionManager 不弹确认
TEST(TaskCompleteTool, IsReadOnly) {
    ToolImpl impl = create_task_complete_tool();
    EXPECT_TRUE(impl.is_read_only);
    EXPECT_EQ(impl.source, acecode::ToolSource::Builtin);
}

// 场景:schema 声明正确 —— summary 必填 + minLength>=1,另有可选的 ask / actions / blocked;
// 描述说明调用时机(做了实事的回合、最终消息之后)。
TEST(TaskCompleteTool, SchemaShape) {
    ToolImpl impl = create_task_complete_tool();
    EXPECT_EQ(impl.definition.name, "task_complete");
    const auto& description = impl.definition.description;
    EXPECT_NE(description.find("\"next step\" box"), std::string::npos);
    EXPECT_NE(description.find("Write your closing message before this call"), std::string::npos);
    EXPECT_NE(description.find("AskUserQuestion"), std::string::npos);

    const auto& params = impl.definition.parameters;
    ASSERT_TRUE(params.contains("required"));
    ASSERT_TRUE(params["required"].is_array());
    ASSERT_EQ(params["required"].size(), 1u);
    EXPECT_EQ(params["required"][0], "summary");

    const auto& properties = params["properties"];
    const auto& summary_schema = properties["summary"];
    EXPECT_EQ(summary_schema["type"], "string");
    EXPECT_GE(summary_schema["minLength"].get<int>(), 1);
    EXPECT_EQ(properties["ask"]["type"], "string");
    EXPECT_EQ(properties["blocked"]["type"], "boolean");
    EXPECT_EQ(properties["actions"]["type"], "array");
    EXPECT_EQ(properties["actions"]["items"]["properties"]["label"]["type"], "string");
}
