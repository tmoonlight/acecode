#include <gtest/gtest.h>

#include "im/text_chunk.hpp"

#include <string>

// im/text_chunk:IM 出站长文本分段。QQ 按字符计、Telegram 按 UTF-16 单位计,
// 分段不能切断 UTF-8 字符,也不能把代码块切成没闭合的半截。

namespace acecode::im {
namespace {

bool valid_utf8(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
        if (len == 0 || i + len > text.size()) return false;
        for (std::size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

std::size_t count_fence_lines(const std::string& text) {
    std::size_t count = 0, pos = 0;
    while ((pos = text.find("```", pos)) != std::string::npos) {
        if (pos == 0 || text[pos - 1] == '\n') ++count;
        pos += 3;
    }
    return count;
}

// 场景:统计中文与 emoji 的长度。
// 期望:按码点计时 emoji 算 1;按 UTF-16 计时增补平面的 emoji 算 2。
TEST(ImTextChunk, CountsCodePointsAndUtf16Units) {
    const std::string text = u8"你好😀";
    EXPECT_EQ(text_units(text, false), 3u);
    EXPECT_EQ(text_units(text, true), 4u);
}

// 场景:短文本、空文本、纯空白。
// 期望:短文本原样一段;空文本与纯空白不产生任何分段(不会发出空消息)。
TEST(ImTextChunk, ShortAndBlankInputs) {
    EXPECT_EQ(chunk_text("hello", 100, false), std::vector<std::string>{"hello"});
    EXPECT_TRUE(chunk_text("", 100, false).empty());
    EXPECT_TRUE(chunk_text(" \n\n ", 100, false).empty());
}

// 场景:两段文字合起来超长,但每段单独都不超长。
// 期望:在空行处切成两段,第二段开头不带多余空行。
TEST(ImTextChunk, PrefersParagraphBoundary) {
    const std::string first(60, 'a');
    const std::string second(60, 'b');
    const auto chunks = chunk_text(first + "\n\n" + second, 100, false);
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0], first);
    EXPECT_EQ(chunks[1], second);
}

// 场景:一整行 500 个汉字,没有任何换行或空格可以切。
// 期望:按字符硬切;每段都是合法 UTF-8 且不超过上限;拼回去与原文完全一致。
TEST(ImTextChunk, HardSplitsLongCjkLineOnCharacterBoundaries) {
    std::string text;
    for (int i = 0; i < 500; ++i) text += u8"汉";
    const auto chunks = chunk_text(text, 120, false);
    ASSERT_GT(chunks.size(), 1u);
    std::string joined;
    for (const auto& chunk : chunks) {
        EXPECT_TRUE(valid_utf8(chunk));
        EXPECT_LE(text_units(chunk, false), 120u);
        joined += chunk;
    }
    EXPECT_EQ(joined, text);
}

// 场景:Telegram 按 UTF-16 计数,40 个 emoji(80 个单位)切成上限 16 的段。
// 期望:每段不超过 16 个 UTF-16 单位,且不切断任何 emoji。
TEST(ImTextChunk, RespectsUtf16LimitForEmoji) {
    std::string text;
    for (int i = 0; i < 40; ++i) text += u8"😀";
    const auto chunks = chunk_text(text, 16, true);
    std::string joined;
    for (const auto& chunk : chunks) {
        EXPECT_LE(text_units(chunk, true), 16u);
        EXPECT_TRUE(valid_utf8(chunk));
        joined += chunk;
    }
    EXPECT_EQ(joined, text);
}

// 场景:一个 python 代码块比单条上限长得多。
// 期望:每段的 ``` 行数都是偶数(围栏成对);后续段以 ```python 重新开头,
// 渲染时每段都是完整的代码块。
TEST(ImTextChunk, KeepsCodeFencesBalancedAcrossChunks) {
    std::string code = "```python\n";
    for (int i = 0; i < 60; ++i) code += "print('line " + std::to_string(i) + "')\n";
    code += "```\n";
    const auto chunks = chunk_text("Intro text\n\n" + code + "\nDone.", 200, false);
    ASSERT_GT(chunks.size(), 2u);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        EXPECT_EQ(count_fence_lines(chunks[i]) % 2, 0u) << "chunk " << i << ":\n" << chunks[i];
        EXPECT_LE(text_units(chunks[i], false), 200u);
    }
    EXPECT_EQ(chunks[1].rfind("```python\n", 0), 0u) << chunks[1];
}

} // namespace
} // namespace acecode::im
