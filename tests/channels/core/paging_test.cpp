#include <gtest/gtest.h>

#include "channels/core/paging.hpp"

// channels/core/paging:IM 回复的分页(纯逻辑)。列表按条数与字节数切页、编号跨页连续,
// 长文本按行切页;页脚给出 /mr 翻页提示。

namespace acecode::channels::core {
namespace {

std::vector<std::string> numbered_rows(std::size_t count) {
    std::vector<std::string> rows;
    for (std::size_t i = 1; i <= count; ++i) rows.push_back("会话 " + std::to_string(i));
    return rows;
}

// 场景:25 条会话按默认每页 10 条分页。
// 期望:共 3 页;编号跨页连续(第二页从 11 开始),/resume 15 才能直接用列表里看到的编号;
// 多页时页脚给出页码与 /mr,最后一页标明。
TEST(ChannelPaging, RowsAreNumberedAcrossPages) {
    const auto output = paginate_rows("可切换的会话:", numbered_rows(25));
    ASSERT_EQ(output.pages.size(), 3u);
    const auto first = render_page(output, 1);
    EXPECT_EQ(first.rfind("可切换的会话:\n1. 会话 1\n", 0), 0u);
    EXPECT_NE(first.find("10. 会话 10"), std::string::npos);
    EXPECT_EQ(first.find("11. "), std::string::npos);
    EXPECT_NE(first.find("第 1/3 页,发送 /mr 看下一页"), std::string::npos);
    EXPECT_EQ(render_page(output, 2).find("可切换的会话:\n11. 会话 11"), 0u);
    const auto last = render_page(output, 3);
    EXPECT_NE(last.find("25. 会话 25"), std::string::npos);
    EXPECT_NE(last.find("(最后一页)"), std::string::npos);
    EXPECT_EQ(render_page(output, 4), "只有 3 页。");
    EXPECT_EQ(render_page(output, 0), "只有 3 页。");
}

// 场景:只有一页内容。
// 期望:不加页脚,避免每条短回复都带「第 1/1 页」。
TEST(ChannelPaging, SinglePageHasNoFooter) {
    const auto output = paginate_rows("列表:", numbered_rows(3));
    ASSERT_EQ(output.pages.size(), 1u);
    EXPECT_EQ(render_page(output, 1), "列表:\n1. 会话 1\n2. 会话 2\n3. 会话 3");
}

// 场景:每条都很长(例如技能描述),10 条加起来超过单条消息的字节上限。
// 期望:按字节上限提前换页,每页正文不超过上限;单条超过上限时仍独占一页、不会丢。
TEST(ChannelPaging, RowsRespectByteLimit) {
    std::vector<std::string> rows(10, std::string(300, 'x'));
    const auto output = paginate_rows({}, rows, 10, 1000);
    EXPECT_GT(output.pages.size(), 1u);
    for (const auto& page : output.pages) EXPECT_LE(page.size(), 1000u);
    const auto oversized = paginate_rows({}, {std::string(2000, 'y'), "短"}, 10, 1000);
    ASSERT_EQ(oversized.pages.size(), 2u);
    EXPECT_EQ(oversized.pages[1], "2. 短");
}

// 场景:内置命令(/memory list 等)输出一大段多行文字。
// 期望:按行切页,每页不超过上限;拼回去不丢内容(切点处的换行除外)。
TEST(ChannelPaging, LongTextSplitsOnLines) {
    std::string text;
    for (int i = 0; i < 200; ++i) text += "第 " + std::to_string(i) + " 行记忆内容\n";
    const auto output = paginate_text({}, text, 500);
    ASSERT_GT(output.pages.size(), 3u);
    std::string joined;
    for (const auto& page : output.pages) {
        EXPECT_LE(page.size(), 500u);
        EXPECT_NE(page.front(), '\n');
        joined += page + "\n";
    }
    EXPECT_NE(joined.find("第 0 行记忆内容"), std::string::npos);
    EXPECT_NE(joined.find("第 199 行记忆内容"), std::string::npos);
    EXPECT_TRUE(paginate_text({}, "").pages.empty());
}

// 场景:会话标题或技能描述里有换行、很长、含中文。
// 期望:折成一行;按字符(不是字节)截断并加「…」,不会切坏 UTF-8;连续空白合并成一个空格。
TEST(ChannelPaging, OneLineTruncatesByCodepoint) {
    EXPECT_EQ(one_line("修复\n登录  页面", 20), "修复 登录 页面");
    EXPECT_EQ(one_line("一二三四五六", 4), "一二三四…");
    EXPECT_EQ(one_line("  前后空白  ", 10), "前后空白");
    EXPECT_EQ(one_line("abc", 3), "abc");
}

} // namespace
} // namespace acecode::channels::core
