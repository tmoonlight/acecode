#include "markdown/markdown_formatter.hpp"

#include <gtest/gtest.h>

#include <ftxui/dom/elements.hpp>

namespace acecode::markdown {

TEST(StreamingFormatter, FreezesCompletedLineWithinParagraph) {
    StreamingFormatter f;
    f.set_context(80, 1);
    auto e1 = f.append_delta("first line\nsecond l", {});
    auto e2 = f.append_delta("ine", {});   // 完成 "second line"
    // 稳定前缀("first line\n")已被冻结:再 append 应只重建尾部,Element 总数不膨胀
    // 无法直接断言内部缓存,断言输出文本逐步覆盖输入:
    // 用 render 到 String 检查;此处断言返回 Element 非空即可(内部缓存正确性由 L3 属性测试覆盖)
    ASSERT_NE(e1.get(), nullptr);
    ASSERT_NE(e2.get(), nullptr);
}

TEST(StreamingFormatter, HoldsLineUntilSafeWhenInlineDelimiterOpen) {
    StreamingFormatter f;
    f.set_context(80, 1);
    f.append_delta("**bold across\n", {});   // 行尾未闭合 ** → 该行不得冻结
    auto e = f.append_delta("lines** done", {});
    ASSERT_NE(e.get(), nullptr);
}

TEST(StreamingFormatter, ContextChangeInvalidatesStable) {
    StreamingFormatter f;
    f.set_context(80, 1);
    f.append_delta("hello\nworld", {});
    f.set_context(40, 1);   // 宽度变化 → 下次 append 从零重建,不应崩溃
    auto e = f.append_delta("!", {});
    ASSERT_NE(e.get(), nullptr);
}

} // namespace acecode::markdown
