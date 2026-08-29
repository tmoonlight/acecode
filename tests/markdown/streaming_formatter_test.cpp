#include "markdown/markdown_formatter.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

namespace acecode::markdown {

namespace {

// FTXUI's ToString() interleaves ANSI escape sequences (colors, bold) between
// styled text runs, so search for literal substrings on the stripped output.
std::string strip_ansi(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        if (in[i] == '\x1b' && i + 1 < in.size() && in[i + 1] == '[') {
            std::size_t j = i + 2;
            while (j < in.size() &&
                   (std::isdigit(static_cast<unsigned char>(in[j])) ||
                    in[j] == ';')) {
                ++j;
            }
            if (j < in.size()) {
                ++j;  // final letter of the CSI sequence
            }
            i = j;
        } else {
            out.push_back(in[i]);
            ++i;
        }
    }
    return out;
}

}  // namespace

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
    // R7: 跨行粗体 **bold across\nlines** done 必须整体渲染为粗体,
    // 不得因首行被冻结而把 ** 当字面量输出。
    f.append_delta("**bold across\n", {});
    auto e = f.append_delta("lines** done", {});
    ftxui::Screen s(80, 20);
    ftxui::Render(s, e);
    const std::string out = strip_ansi(s.ToString());
    // 若首行被冻结,稳定区会以字面 "**bold" 开头渲染(输出含 "**bold" 拆分);
    // 正确实现:整段作粗体渲染,输出不含字面 **,但文本完整。
    EXPECT_EQ(out.find("**bold"), std::string::npos);
    EXPECT_NE(out.find("bold across"), std::string::npos);
    EXPECT_NE(out.find("lines done"), std::string::npos);
}

TEST(StreamingFormatter, CodeFenceWithLanguageStaysUnfrozen) {
    // R7: 带语言标签的开围栏 ```cpp 必须被识别为围栏,内部行不得冻结。
    StreamingFormatter f;
    f.set_context(80, 1);
    auto e = f.append_delta("before\n\n```cpp\nint x = 1;\n", {});
    auto e2 = f.append_delta("int y = 2;\n```\nafter\n", {});
    ftxui::Screen s(80, 20);
    ftxui::Render(s, e2);
    const std::string out = strip_ansi(s.ToString());
    EXPECT_NE(out.find("int x"), std::string::npos);
    EXPECT_NE(out.find("int y"), std::string::npos);
    EXPECT_NE(out.find("after"), std::string::npos);
}

TEST(StreamingFormatter, ContextChangeInvalidatesStable) {
    StreamingFormatter f;
    f.set_context(80, 1);
    f.append_delta("hello\nworld", {});
    f.set_context(40, 1);   // 宽度变化 → 下次 append 从零重建,不应崩溃
    auto e = f.append_delta("!", {});
    ASSERT_NE(e.get(), nullptr);
}

TEST(StreamingFormatter, WidthChangeKeepsAccumulatedContent) {
    // R11(P1): 宽度 fallback 分支不得丢弃已累积流式文本。set_context 只清
    // 稳定渲染缓存,保留 full_content_;下次 append 用默认宽度(80)与存储
    // 宽度(40)不一致 → 触发 fallback → 用 full_content_ 重放重建。
    StreamingFormatter f;
    f.set_context(80, 1);
    f.append_delta("hello\nworld\n", {});
    f.set_context(40, 1);
    auto e = f.append_delta("more\n", {});
    ftxui::Screen s(80, 20);
    ftxui::Render(s, e);
    const std::string out = strip_ansi(s.ToString());
    EXPECT_NE(out.find("hello"), std::string::npos);
    EXPECT_NE(out.find("world"), std::string::npos);
    EXPECT_NE(out.find("more"), std::string::npos);
}

TEST(StreamingFormatter, StableElementsBuiltOncePerNewToken) {
    StreamingFormatter f; f.set_context(80, 1);
    // 多块流式:稳定区只增量构建,不整段重建。若稳定区缓存被误清空
    // (例如把 stable_elements_ move 进 vbox),第二次 append 会丢失前面段落,
    // 因此渲染结果必须仍覆盖全部多段内容。
    auto e = f.append_delta("a\n\nb\n\nc\n", {});
    ASSERT_NE(e.get(), nullptr);
    auto e2 = f.append_delta("d\n", {});
    ASSERT_NE(e2.get(), nullptr);
    ftxui::Screen s(80, 20);
    ftxui::Render(s, e2);
    const std::string out = strip_ansi(s.ToString());
    EXPECT_NE(out.find("a"), std::string::npos);
    EXPECT_NE(out.find("b"), std::string::npos);
    EXPECT_NE(out.find("c"), std::string::npos);
    EXPECT_NE(out.find("d"), std::string::npos);
}

} // namespace acecode::markdown
