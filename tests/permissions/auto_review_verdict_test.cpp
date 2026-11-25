// 自动模式（试验）裁决回答的解析(openspec add-auto-review-mode,规格「回答的解析」)。
// 这里的结论直接决定一次工具调用是放行、拦下还是「没有结果」:
//   - 把思考里的自言自语当成结论,会放行或拦下本不该如此的操作;
//   - 把格式错误记成拦下,会让弱模型的每次跑偏都计入拦截次数、最后把回合锁死。

#include <gtest/gtest.h>

#include "permissions/auto_review_verdict.hpp"

using acecode::auto_review::parse_verdict;
using Kind = acecode::auto_review::ParsedVerdict::Kind;

// 场景:裁决模型按要求只回了标记。
// 期望:no → Allow;yes → Block,并取出 <reason> 里的一句话。
TEST(AutoReviewVerdict, ParsesPlainAnswers) {
    EXPECT_EQ(parse_verdict("<block>no</block>").kind, Kind::Allow);

    const auto blocked = parse_verdict(
        "<block>yes</block><reason>B2: force-push rewrites published history</reason>");
    EXPECT_EQ(blocked.kind, Kind::Block);
    EXPECT_EQ(blocked.reason, "B2: force-push rewrites published history");
}

// 场景:模型在思考块里先写了一个相反的标记,思考结束后才给结论(规格里的原例)。
// 期望:思考块整体剔除,只认块外的结论。回归意义:不剔除的话这份回答里 yes / no
// 都出现,会被判成无法解析,或者更糟 —— 被思考里的那个标记带偏。
TEST(AutoReviewVerdict, IgnoresMarkersInsideThinkingBlocks) {
    EXPECT_EQ(parse_verdict("<think>也许应该 <block>yes</block></think><block>no</block>").kind,
              Kind::Allow);
    EXPECT_EQ(parse_verdict("<thinking>looks fine, <block>no</block>?</thinking>\n"
                            "<block>yes</block><reason>B1</reason>").kind,
              Kind::Block);
    // 大小写不同的标签同样识别。
    EXPECT_EQ(parse_verdict("<THINK><block>yes</block></THINK><Block>NO</Block>").kind,
              Kind::Allow);
}

// 场景:输出被截断,思考块只有开标签没有闭标签。
// 期望:从开标签一直剔到末尾,里面的标记不算数 → 整份回答没有结论 → Invalid。
TEST(AutoReviewVerdict, UnclosedThinkingBlockSwallowsTheRest) {
    EXPECT_EQ(parse_verdict("<think>the command deletes... <block>yes</block>").kind, Kind::Invalid);
    // 结论在未闭合的思考块之前:仍然有效。
    EXPECT_EQ(parse_verdict("<block>no</block><think>second thoughts").kind, Kind::Allow);
}

// 场景:同一份回答里 yes 和 no 都出现(规格「自相矛盾的回答」)。
// 期望:Invalid,而不是取第一个或最后一个。
TEST(AutoReviewVerdict, ContradictoryAnswerIsInvalid) {
    EXPECT_EQ(parse_verdict("<block>yes</block> ... on reflection <block>no</block>").kind,
              Kind::Invalid);
}

// 场景:模型没有按格式回答 —— 一段自然语言、空串、或只有标签没有取值。
// 期望:全部 Invalid。调用方把它当「没有结果」,不计入拦截次数。
TEST(AutoReviewVerdict, MissingMarkerIsInvalid) {
    EXPECT_EQ(parse_verdict("This action looks safe to me.").kind, Kind::Invalid);
    EXPECT_EQ(parse_verdict("").kind, Kind::Invalid);
    EXPECT_EQ(parse_verdict("<block></block>").kind, Kind::Invalid);
    EXPECT_EQ(parse_verdict("<block>maybe</block>").kind, Kind::Invalid);
}

// 场景:取值后面紧跟别的字母(`<block>none`、`<block>yesterday`),以及标签
// 内带空白、没有闭标签的写法。
// 期望:前两种不是合法取值 → Invalid;空白与缺闭标签容忍 —— provider 没有停止
// 序列时模型常常少写一个闭标签,不该因此丢掉一个明确的结论。
TEST(AutoReviewVerdict, RequiresWholeWordButToleratesLooseFormatting) {
    EXPECT_EQ(parse_verdict("<block>none</block>").kind, Kind::Invalid);
    EXPECT_EQ(parse_verdict("<block>yesterday</block>").kind, Kind::Invalid);
    EXPECT_EQ(parse_verdict("<block> no </block>").kind, Kind::Allow);
    EXPECT_EQ(parse_verdict("<block>yes").kind, Kind::Block);
    EXPECT_EQ(parse_verdict("<block>no").kind, Kind::Allow);
    // 同一个取值重复出现不算矛盾。
    EXPECT_EQ(parse_verdict("<block>no</block>\n<block>no</block>").kind, Kind::Allow);
}

// 场景:理由写成多行、带多余空白,或者长到离谱,或者 <reason> 没有闭合。
// 期望:折成单行并去掉两端空白;超过 400 字节的在 UTF-8 字符边界截断并加省略号
// (400 是「一句话」的宽松上限,理由会原样回给主模型和写进审计,不能任它无限长);
// 没闭合的取到末尾。放行时不带理由。
TEST(AutoReviewVerdict, TidiesTheReason) {
    EXPECT_EQ(parse_verdict("<block>yes</block><reason>\n  B1:\tdeletes\n  uncommitted work \n</reason>").reason,
              "B1: deletes uncommitted work");
    EXPECT_EQ(parse_verdict("<block>yes</block><reason>cut off here").reason, "cut off here");
    EXPECT_TRUE(parse_verdict("<block>yes</block>").reason.empty());
    EXPECT_TRUE(parse_verdict("<block>no</block><reason>ignored</reason>").reason.empty());

    std::string long_reason;
    for (int i = 0; i < 200; ++i) long_reason += "拦";   // 600 字节,每个字 3 字节
    const auto clipped = parse_verdict("<block>yes</block><reason>" + long_reason + "</reason>").reason;
    EXPECT_LE(clipped.size(), 403u);
    EXPECT_EQ(clipped.substr(clipped.size() - 3), "...");
    // 截断点落在字符边界上:去掉省略号后的长度是 3 的倍数。
    EXPECT_EQ((clipped.size() - 3) % 3, 0u);
}
