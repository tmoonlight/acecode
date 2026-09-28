#include "markdown/markdown_formatter.hpp"
#include "markdown/markdown_lexer.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <string>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

namespace acecode::markdown {

namespace {

// FTXUI's ToString() interleaves ANSI escape sequences (colors, bold) between
// styled text runs, so strip them before comparing rendered output.
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

std::string render_text(const ftxui::Element& element, int width, int height) {
    ftxui::Screen screen(width, height);
    ftxui::Render(screen, element);
    return strip_ansi(screen.ToString());
}

}  // namespace

TEST(RenderTokenBlocks, MatchesFormatMarkdownOutput) {
    FormatOptions opts;
    opts.terminal_width = 60;
    opts.syntax_highlight = true;

    const std::string md =
        "# Hi\n\nsome **bold** text\n\n```cpp\nint x;\n```\n";

    auto tokens = lex(md);
    auto e1 = render_token_blocks(tokens, opts);
    auto e2 = format_markdown(md, opts);

    ASSERT_NE(e1.get(), nullptr);
    ASSERT_NE(e2.get(), nullptr);

    // 结构等价:同宽度下同布局 → 渲染输出文本与尺寸一致。
    const std::string t1 = render_text(e1, 60, 20);
    const std::string t2 = render_text(e2, 60, 20);
    EXPECT_EQ(t1, t2);
    EXPECT_NE(t1.find("Hi"), std::string::npos);
    EXPECT_NE(t1.find("bold"), std::string::npos);
    EXPECT_NE(t1.find("int x;"), std::string::npos);
}

TEST(RenderTokenBlocks, EmptyTokenListYieldsEmptyElement) {
    FormatOptions opts;
    auto e = render_token_blocks({}, opts);
    ASSERT_NE(e.get(), nullptr);
}

} // namespace acecode::markdown
