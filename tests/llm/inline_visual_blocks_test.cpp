// 覆盖 src/domain/llm/inline_visual_blocks.cpp(openspec add-inline-visuals):IM 与 TUI
// 画不了 visual,发出 / 显示前把顶层 ```visual 代码块换成一行占位提示。
//   1. 多块按顺序替换,前后正文与 mermaid 代码块原样保留
//   2. 未闭合(回复中断 / 截断)时一直替换到正文末尾
//   3. 嵌套在列表、引用里的 visual 在 Web 上按普通代码显示,这里也不动
//   4. 其它语言代码块里的 ```visual 字样不算(按 CommonMark 围栏规则跳过整块)
//   5. 信息串第一个词不区分大小写;~~~ 围栏与更长的反引号围栏同样识别

#include <gtest/gtest.h>

#include "llm/inline_visual_blocks.hpp"

#include <string>

using acecode::find_inline_visual_blocks;
using acecode::is_inline_visual_info;
using acecode::replace_inline_visual_blocks;

namespace {
const std::string kHint = "[查看提示]";
}

// 场景:回复是「说明 + visual + mermaid + visual + 结论」。
// 期望:两个 visual 都换成提示,mermaid 与说明文字一个字节都不变。
TEST(InlineVisualBlocks, ReplacesEveryTopLevelBlockAndKeepsMermaid) {
    const std::string text =
        "说明文字\n\n```visual\n<svg viewBox=\"0 0 10 10\"></svg>\n```\n\n"
        "```mermaid\nflowchart TD\n  A --> B\n```\n\n"
        "```visual\n<div class=\"card\">卡片</div>\n```\n结论";
    const auto blocks = find_inline_visual_blocks(text);
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_TRUE(blocks[0].closed);
    EXPECT_TRUE(blocks[1].closed);
    EXPECT_EQ(replace_inline_visual_blocks(text, kHint),
              "说明文字\n\n" + kHint + "\n\n```mermaid\nflowchart TD\n  A --> B\n```\n\n" +
                  kHint + "\n结论");
}

// 场景:模型输出到一半被中断,visual 代码块缺少结尾围栏。
// 期望:识别为未闭合的块,从起始围栏一直替换到正文末尾,不漏出半截 HTML。
TEST(InlineVisualBlocks, UnclosedBlockRunsToTheEnd) {
    const std::string text = "前言\n```visual\n<svg>\n<rect x=\"1\"";
    const auto blocks = find_inline_visual_blocks(text);
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_FALSE(blocks[0].closed);
    EXPECT_EQ(blocks[0].end, text.size());
    EXPECT_EQ(replace_inline_visual_blocks(text, kHint), "前言\n" + kHint);
}

// 场景:visual 代码块缩进在列表项里,或写在引用块里。
// 期望:Web 上它们按普通代码显示,这里也原样保留,不替换。
TEST(InlineVisualBlocks, NestedBlocksStayAsCode) {
    const std::string in_list = "- 第一项\n  ```visual\n  <svg></svg>\n  ```\n- 第二项\n";
    EXPECT_TRUE(find_inline_visual_blocks(in_list).empty());
    EXPECT_EQ(replace_inline_visual_blocks(in_list, kHint), in_list);

    const std::string in_quote = "> 引用\n> ```visual\n> <svg></svg>\n> ```\n";
    EXPECT_TRUE(find_inline_visual_blocks(in_quote).empty());

    // 列表之后空一行、回到顶层的 visual 照常识别。
    const std::string after_list = "- 一项\n\n```visual\n<svg></svg>\n```\n";
    ASSERT_EQ(find_inline_visual_blocks(after_list).size(), 1u);
}

// 场景:模型在 ```markdown 代码块里演示 visual 的写法,或用四个空格缩进的代码块展示。
// 期望:都不算 visual(内层 ```visual 只是代码内容 / 缩进代码),原样保留。
TEST(InlineVisualBlocks, IgnoresVisualTextInsideOtherCodeBlocks) {
    const std::string demo =
        "````markdown\n```visual\n<svg></svg>\n```\n````\n\n    ```visual\n    <svg></svg>\n";
    EXPECT_TRUE(find_inline_visual_blocks(demo).empty());
    EXPECT_EQ(replace_inline_visual_blocks(demo, kHint), demo);
}

// 场景:信息串写成 Visual / VISUAL 或带额外参数,围栏用 ~~~ 或更长的反引号,行尾是 \r\n。
// 期望:都识别;结束围栏必须同字符、长度不短于开头;html / svg / visualize 不算。
TEST(InlineVisualBlocks, InfoStringAndFenceVariants) {
    EXPECT_TRUE(is_inline_visual_info("visual"));
    EXPECT_TRUE(is_inline_visual_info(" Visual title=\"x\""));
    EXPECT_TRUE(is_inline_visual_info("VISUAL{.wide}"));
    EXPECT_FALSE(is_inline_visual_info("html"));
    EXPECT_FALSE(is_inline_visual_info("svg"));
    EXPECT_FALSE(is_inline_visual_info("visualize"));
    EXPECT_FALSE(is_inline_visual_info(""));

    const std::string tilde = "~~~Visual\n<svg></svg>\n```\nstill inside\n~~~\nafter";
    const auto blocks = find_inline_visual_blocks(tilde);
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_TRUE(blocks[0].closed);
    EXPECT_EQ(replace_inline_visual_blocks(tilde, kHint), kHint + "\nafter");

    const std::string crlf = "a\r\n````visual\r\n<svg></svg>\r\n```\r\n````\r\nb";
    EXPECT_EQ(replace_inline_visual_blocks(crlf, kHint), "a\r\n" + kHint + "\r\nb");
}

// 场景:没有任何 visual 的普通回复(最常见的情况)。期望:原样返回。
TEST(InlineVisualBlocks, PlainTextIsUntouched) {
    const std::string text = "普通回复\n```cpp\nint main() {}\n```\n";
    EXPECT_TRUE(find_inline_visual_blocks(text).empty());
    EXPECT_EQ(replace_inline_visual_blocks(text, kHint), text);
}
