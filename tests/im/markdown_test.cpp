#include <gtest/gtest.h>

#include "im/markdown.hpp"

#include <string>

// im/markdown:助手回复在 Telegram 上以 HTML 子集发送,在 QQ Markdown 被拒时退回纯文本。
// 关键约束:Telegram 的 HTML 解析很严格,标签必须成对,文本里的 < > & 必须转义。

namespace acecode::im {
namespace {

std::size_t count(const std::string& text, const std::string& needle) {
    std::size_t n = 0, pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

// 场景:代码块里含有 < > & 和看起来像 Markdown 的 **x**。
// 期望:整体放进 <pre><code class="language-cpp">,内容只做转义、不做任何格式化。
TEST(ImMarkdown, CodeBlockIsEscapedVerbatim) {
    const auto html = markdown_to_telegram_html("```cpp\nif (a < b && **x**) {}\n```");
    EXPECT_EQ(html, "<pre><code class=\"language-cpp\">if (a &lt; b &amp;&amp; **x**) {}</code></pre>");
}

// 场景:行内代码、粗体、斜体、删除线混排。
// 期望:分别转成 code/b/i/s 标签,行内代码里的 < 被转义。
TEST(ImMarkdown, InlineFormatting) {
    EXPECT_EQ(markdown_to_telegram_html("use `a<b` and **bold** *it* ~~old~~"),
              "use <code>a&lt;b</code> and <b>bold</b> <i>it</i> <s>old</s>");
}

// 场景:snake_case 标识符和 2 * 3 * 4 这样的算式里出现 _ 和 *。
// 期望:不被误判成斜体,原样输出。
TEST(ImMarkdown, IdentifiersAndArithmeticAreNotEmphasis) {
    EXPECT_EQ(markdown_to_telegram_html("call my_long_function_name now"),
              "call my_long_function_name now");
    EXPECT_EQ(markdown_to_telegram_html("2 * 3 * 4"), "2 * 3 * 4");
}

// 场景:安全链接与 javascript: 链接。
// 期望:http(s) 链接转成 <a href>;javascript: 链接不生成标签,按文字输出。
TEST(ImMarkdown, OnlySafeLinksBecomeAnchors) {
    EXPECT_EQ(markdown_to_telegram_html("see [docs](https://example.com/a?b=1&c=2)"),
              "see <a href=\"https://example.com/a?b=1&amp;c=2\">docs</a>");
    EXPECT_EQ(markdown_to_telegram_html("[x](javascript:alert(1))"), "[x](javascript:alert(1))");
}

// 场景:标题、无序列表、有序列表、引用、分隔线。
// 期望:标题变粗体,列表变“• ”,有序编号保留,连续引用行合并进一个 blockquote。
TEST(ImMarkdown, BlockElements) {
    const auto html = markdown_to_telegram_html("# Title\n- one\n2. two\n> q1\n> q2\n---");
    EXPECT_EQ(html, "<b>Title</b>\n• one\n2. two\n<blockquote>q1\nq2</blockquote>\n——————");
}

// 场景:未闭合的粗体标记、孤立的反引号、普通文本里的尖括号。
// 期望:按字面输出并转义;生成的 HTML 里开闭标签数量一致。
TEST(ImMarkdown, UnclosedMarkersStayLiteralAndTagsBalance) {
    const auto html = markdown_to_telegram_html("**open and `tick <tag> & more");
    EXPECT_EQ(html, "**open and `tick &lt;tag&gt; &amp; more");
    const auto mixed = markdown_to_telegram_html("**a *b* c** and __d__ `e` ~~f~~");
    EXPECT_EQ(count(mixed, "<b>"), count(mixed, "</b>"));
    EXPECT_EQ(count(mixed, "<i>"), count(mixed, "</i>"));
    EXPECT_EQ(count(mixed, "<s>"), count(mixed, "</s>"));
}

// 场景:表格。
// 期望:整张表放进 <pre> 保持对齐,单元格里的 < 被转义。
TEST(ImMarkdown, TablesBecomePreformatted) {
    EXPECT_EQ(markdown_to_telegram_html("| a | b |\n|---|---|\n| 1 | <2 |"),
              "<pre>| a | b |\n|---|---|\n| 1 | &lt;2 |</pre>");
}

// 场景:QQ Markdown 被拒后回退纯文本。
// 期望:去掉粗体/代码围栏/标题标记,链接变成“文字 (地址)”,不做 HTML 转义。
TEST(ImMarkdown, PlainFallbackStripsMarkers) {
    EXPECT_EQ(markdown_to_plain("# 标题\n**粗体** 和 [链接](https://a.b)\n```\nx < y\n```"),
              "标题\n粗体 和 链接 (https://a.b)\nx < y");
}

} // namespace
} // namespace acecode::im
