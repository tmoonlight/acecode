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

std::string render_text(const ftxui::Element& element, int width = 80) {
    ftxui::Screen screen(width, 24);
    ftxui::Render(screen, element);
    return strip_ansi(screen.ToString());
}

void expect_chunked_matches_complete(const std::string& full) {
    StreamingFormatter formatter;
    FormatOptions opts;
    opts.terminal_width = 80;
    std::string prefix;
    for (char c : full) {
        prefix.push_back(c);
        auto streamed = formatter.append_delta(std::string(1, c), opts);
        SCOPED_TRACE(prefix.size());
        EXPECT_EQ(render_text(streamed),
                  render_text(format_markdown(prefix, opts)));
    }
}

}  // namespace

TEST(StreamingFormatter, RendersAccumulatedParagraph) {
    StreamingFormatter f;
    auto e1 = f.append_delta("first line\nsecond l", {});
    auto e2 = f.append_delta("ine", {});
    ASSERT_NE(e1.get(), nullptr);
    ASSERT_NE(e2.get(), nullptr);
}

TEST(StreamingFormatter, CrossLineInlineDelimiterMatchesCompleteFormatting) {
    StreamingFormatter f;
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

TEST(StreamingFormatter, FencedCodeKeepsAccumulatedContent) {
    StreamingFormatter f;
    auto e = f.append_delta("before\n\n```cpp\nint x = 1;\n", {});
    auto e2 = f.append_delta("int y = 2;\n```\nafter\n", {});
    ftxui::Screen s(80, 20);
    ftxui::Render(s, e2);
    const std::string out = strip_ansi(s.ToString());
    EXPECT_NE(out.find("int x"), std::string::npos);
    EXPECT_NE(out.find("int y"), std::string::npos);
    EXPECT_NE(out.find("after"), std::string::npos);
}

TEST(StreamingFormatter, OptionWidthChangeKeepsAccumulatedContent) {
    StreamingFormatter f;
    FormatOptions opts;
    opts.terminal_width = 80;
    f.append_delta("hello\nworld\n", opts);
    opts.terminal_width = 40;
    auto e = f.append_delta("more\n", opts);
    ftxui::Screen s(80, 20);
    ftxui::Render(s, e);
    const std::string out = strip_ansi(s.ToString());
    EXPECT_NE(out.find("hello"), std::string::npos);
    EXPECT_NE(out.find("world"), std::string::npos);
    EXPECT_NE(out.find("more"), std::string::npos);
}

TEST(StreamingFormatter, KeepsAllAccumulatedContent) {
    StreamingFormatter f;
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

TEST(StreamingFormatter, ChunkedParagraphMatchesCompleteFormatting) {
    expect_chunked_matches_complete("hello\nworld");
}

TEST(StreamingFormatter, ChunkedTableMatchesCompleteFormatting) {
    expect_chunked_matches_complete(
        "| A | B |\n| --- | --- |\n| 1 | 2 |\n");
}

TEST(StreamingFormatter, ChunkedListContinuationMatchesCompleteFormatting) {
    expect_chunked_matches_complete("- first line\n  continuation\n");
}

TEST(StreamingFormatter, ChunkedLazyBlockquoteMatchesCompleteFormatting) {
    expect_chunked_matches_complete("> quoted\nlazy continuation\n");
}

TEST(StreamingFormatter, SplitXmlWrapperMatchesCompleteMessageFormatting) {
    StreamingFormatter f;
    FormatOptions opts;
    opts.terminal_width = 80;
    opts.strip_xml = true;

    f.append_delta("<thinking>secret", opts);
    const std::string full = "<thinking>secret</thinking>\nvisible answer";
    auto streamed = f.append_delta("</thinking>\nvisible answer", opts);
    auto complete = format_markdown(full, opts);

    const std::string streamed_text = render_text(streamed);
    const std::string complete_text = render_text(complete);

    EXPECT_EQ(streamed_text, complete_text);
    EXPECT_EQ(streamed_text.find("secret"), std::string::npos);
    EXPECT_NE(streamed_text.find("visible answer"), std::string::npos);
}

} // namespace acecode::markdown
