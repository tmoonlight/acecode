#include "markdown/markdown_lexer.hpp"
#include <gtest/gtest.h>
namespace acecode::markdown {
// 属性测试 A:块边界内容,增量==全量
TEST(LexerState, IncrementalEqualsFullAtBlockBoundaries) {
    const std::string full = "para one\n\npara two\n\n```cpp\nint x;\n```\n";
    LexerState st;
    for (char c : full) { st.append(std::string(1, c)); }   // 逐字符喂
    std::vector<Token> incr = st.stable_tokens();
    auto tail = st.tail_tokens();
    incr.insert(incr.end(), tail.begin(), tail.end());
    auto ref = lex(full);
    ASSERT_EQ(incr.size(), ref.size());
    for (std::size_t i = 0; i < ref.size(); ++i) {
        EXPECT_EQ(incr[i].type, ref[i].type);
        EXPECT_EQ(incr[i].text, ref[i].text);
        EXPECT_EQ(incr[i].raw, ref[i].raw);
    }
}
// 属性测试 B:未闭合代码围栏留在 pending(不提前冻结)
TEST(LexerState, OpenFenceStaysInPending) {
    LexerState st;
    st.append("```cpp\nint x = 1;\n");   // 围栏未闭合
    EXPECT_TRUE(st.stable_tokens().empty());
    st.append("```\n");                   // 闭合 → 稳定
    EXPECT_FALSE(st.stable_tokens().empty());
}
// 属性测试 C:含未闭合行内分隔符的行不冻结
TEST(LexerState, UnsafeLineStaysPending) {
    LexerState st;
    st.append("**bold across\n");        // 行尾未闭合 **
    EXPECT_TRUE(st.stable_tokens().empty());
    st.append("lines**\n");
    EXPECT_FALSE(st.stable_tokens().empty());
}
// 属性测试 D:reset 清空
TEST(LexerState, ResetClears) {
    LexerState st;
    st.append("hello\n");
    EXPECT_FALSE(st.stable_tokens().empty());
    st.reset();
    EXPECT_TRUE(st.stable_tokens().empty());
    EXPECT_TRUE(st.tail_tokens().empty());
}
} // namespace acecode::markdown
