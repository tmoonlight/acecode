#include <gtest/gtest.h>

#include "channels/core/history_text.hpp"

// channels/core/history_text:IM 里 /history 回看的排版(纯逻辑)。最新的一轮在前,
// 每轮内部「我 → ACECode」按时间顺序;只放文字,工具调用只计次数。

namespace acecode::channels::core {
namespace {

ChatMessage msg(const std::string& role, const std::string& content, const std::string& timestamp = {}) {
    ChatMessage message;
    message.role = role;
    message.content = content;
    message.timestamp = timestamp;
    return message;
}

std::string all_pages(const PagedOutput& output) {
    std::string text = output.heading;
    for (const auto& page : output.pages) text += "\n" + page;
    return text;
}

// 场景:会话里有三轮对话,用户要看最近两轮。
// 期望:只出最近两轮,最新的一轮在前、标「倒数第 1 轮」;轮内先「我」后「ACECode」;
// 时间按本地时区(UTC+8)显示成「月-日 时:分」;标题说明是最近 2 轮。
TEST(ChannelHistoryText, ShowsNewestTurnsFirst) {
    const std::vector<ChatMessage> messages = {
        msg("user", "问题一", "2026-11-04T01:00:00Z"), msg("assistant", "回答一"),
        msg("user", "问题二", "2026-11-04T02:00:00Z"), msg("assistant", "回答二"),
        msg("user", "问题三", "2026-11-04T06:05:00Z"), msg("assistant", "回答三"),
    };
    const auto output = render_recent_turns(messages, 2, 480, false);
    const auto text = all_pages(output);
    EXPECT_NE(output.heading.find("最近 2 轮"), std::string::npos);
    EXPECT_EQ(text.find("问题一"), std::string::npos);
    const auto newest = text.find("【倒数第 1 轮 · 11-04 14:05】");
    const auto older = text.find("【倒数第 2 轮 · 11-04 10:00】");
    ASSERT_NE(newest, std::string::npos);
    ASSERT_NE(older, std::string::npos);
    EXPECT_LT(newest, older);
    EXPECT_LT(text.find("我:问题三"), text.find("ACECode:回答三"));
    EXPECT_LT(text.find("ACECode:回答三"), text.find("我:问题二"));
}

// 场景:一轮里模型先说了一段话、调用了三次工具、最后给出结论;中间夹着工具结果、
// 隐藏的目标上下文、压缩摘要、蜂群内部消息与一条系统通知;用户消息带 display_text。
// 期望:用户一侧显示 display_text;ACECode 一侧依次放两段文字;工具结果与内部消息都不显示,
// 只说明调用了 3 次工具;系统通知折成一行带「[系统]」;内部消息不算新的一轮。
TEST(ChannelHistoryText, KeepsOnlyReadableText) {
    auto user = msg("user", "[展开后的提示词] 修一下登录");
    user.metadata = {{"display_text", "修一下登录"}};
    auto first = msg("assistant", "我先看看代码。");
    first.tool_calls = nlohmann::json::array({{{"id", "a"}}, {{"id", "b"}}});
    auto goal = msg("user", "继续推进目标");
    goal.metadata = {{"hidden_goal_context", true}};
    auto summary = msg("user", "之前的对话摘要");
    summary.is_compact_summary = true;
    auto inter = msg("user", "<inter_agent_message>子任务完成</inter_agent_message>");
    inter.metadata = {{"inter_agent", {{"type", "final_answer"}}}};
    auto second = msg("assistant", "");
    second.tool_calls = nlohmann::json::array({{{"id", "c"}}});
    const std::vector<ChatMessage> messages = {
        user, first, msg("tool", "文件内容很长"), goal, summary, inter, second, msg("tool", "测试通过"),
        msg("system", "已切换模型\n为 smart"), msg("assistant", "已经修好了。"),
    };
    const auto text = all_pages(render_recent_turns(messages, 5, 0, false));
    EXPECT_NE(text.find("只有 1 轮"), std::string::npos);
    EXPECT_NE(text.find("我:修一下登录"), std::string::npos);
    EXPECT_EQ(text.find("展开后的提示词"), std::string::npos);
    EXPECT_NE(text.find("ACECode:我先看看代码。\n\n已经修好了。"), std::string::npos);
    EXPECT_EQ(text.find("文件内容很长"), std::string::npos);
    EXPECT_EQ(text.find("继续推进目标"), std::string::npos);
    EXPECT_EQ(text.find("之前的对话摘要"), std::string::npos);
    EXPECT_EQ(text.find("子任务完成"), std::string::npos);
    EXPECT_NE(text.find("[系统] 已切换模型 为 smart"), std::string::npos);
    EXPECT_NE(text.find("调用了 3 次工具"), std::string::npos);
}

// 场景:读到的消息从某一轮中间开始(第一条是助手消息),且会话正在执行最新一轮、还没有文字回复。
// 期望:开头那段不完整的消息丢弃;最新一轮标「执行中」并说明还没有文字回复;
// 没有任何轮次时给出「还没有对话内容」且没有正文页。
TEST(ChannelHistoryText, PartialHeadRunningTurnAndEmptySession) {
    const std::vector<ChatMessage> messages = {msg("assistant", "上一轮的尾巴"), msg("user", "新问题")};
    const auto text = all_pages(render_recent_turns(messages, 1, 0, true));
    EXPECT_EQ(text.find("上一轮的尾巴"), std::string::npos);
    EXPECT_NE(text.find("执行中】"), std::string::npos);
    EXPECT_NE(text.find("还在执行,暂时没有文字回复"), std::string::npos);
    const auto empty = render_recent_turns({msg("assistant", "孤立的回复")}, 1, 0, false);
    EXPECT_NE(empty.heading.find("还没有对话内容"), std::string::npos);
    EXPECT_TRUE(empty.pages.empty());
}

// 场景:最新一轮的回复本身就超过整段上限,另有一轮更早的对话。
// 期望:最新一轮在 UTF-8 边界截断并注明;更早的一轮放不下,给出省略说明;
// 长文本切成多页(每页不超过分页字节上限),由 /more 翻看。
// 阈值:kMaxHistoryBytes(192 KiB)防止一次回看在内存和 IM 里堆出上百页。
TEST(ChannelHistoryText, OversizedOutputIsCutAndPaged) {
    std::string huge;
    while (huge.size() < kMaxHistoryBytes + 1000) huge += "很长的输出内容\n";
    const std::vector<ChatMessage> messages = {msg("user", "旧问题"), msg("assistant", "旧回答"),
                                               msg("user", "新问题"), msg("assistant", huge)};
    const auto output = render_recent_turns(messages, 2, 0, false);
    const auto text = all_pages(output);
    EXPECT_NE(text.find("已截断"), std::string::npos);
    EXPECT_NE(text.find("更早的 1 轮内容太长,已省略"), std::string::npos);
    EXPECT_EQ(text.find("旧回答"), std::string::npos);
    EXPECT_GT(output.pages.size(), 10u);
    for (const auto& page : output.pages) EXPECT_LE(page.size(), kPageMaxBytes);
}

// 场景:用户从 IM 只发了一张图(没有文字),另一轮发了文字加一个文件。
// 期望:只有图片时写明附带了图片或文件,不留空的「我:」;有文字时在文字后补充附件个数。
TEST(ChannelHistoryText, AttachmentsAreCounted) {
    auto image_only = msg("user", "");
    image_only.content_parts = nlohmann::json::array({{{"type", "image"}}});
    auto with_file = msg("user", "看看这个");
    with_file.content_parts = nlohmann::json::array({{{"type", "text"}, {"text", "看看这个"}}, {{"type", "file"}}});
    const auto text = all_pages(render_recent_turns({image_only, msg("assistant", "收到图"), with_file}, 2, 0, false));
    EXPECT_NE(text.find("我:(附带 1 个图片或文件)"), std::string::npos);
    EXPECT_NE(text.find("我:看看这个 (附带 1 个图片或文件)"), std::string::npos);
}

// 场景:会话时间是 UTC 的 ISO 8601,本地时区跨日;以及格式不对的时间。
// 期望:换算到本地日期与时刻;格式不对时原样返回。
TEST(ChannelHistoryText, LocalTimeCrossesMidnight) {
    EXPECT_EQ(short_local_time("2026-12-31T20:30:00Z", 480), "01-01 04:30");
    EXPECT_EQ(short_local_time("2026-03-01T01:00:00Z", -120), "02-28 23:00");
    EXPECT_EQ(short_local_time("昨天", 480), "昨天");
}

} // namespace
} // namespace acecode::channels::core
