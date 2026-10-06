#include <gtest/gtest.h>

#include "im/telegram/tg_update.hpp"

// im/telegram/tg_update:Telegram 更新解析。重点是群聊“是否点名机器人”的判定,
// 以及 entity 偏移按 UTF-16 计、要换算成 UTF-8 字节的问题。

namespace acecode::im::telegram {
namespace {

const BotIdentity kBot{"8001", "AceTestBot"};

nlohmann::json message(nlohmann::json chat, nlohmann::json extra) {
    nlohmann::json m{{"message_id", 55}, {"from", {{"id", 42}, {"is_bot", false}, {"first_name", "Ann"}}},
                     {"chat", std::move(chat)}};
    for (auto& [k, v] : extra.items()) m[k] = v;
    return {{"update_id", 7}, {"message", m}};
}

nlohmann::json group_chat() { return {{"id", -1001}, {"type", "supergroup"}}; }

// 场景:私聊里发 "/start abc123"(机主绑定链接带的码)。
// 期望:视为点名,地址为私聊(chat 与 sender 都是对方 id),提取出绑定码,回复上下文带 message_id。
TEST(TelegramUpdate, PrivateStartCarriesBindingCode) {
    const auto parsed = parse_update(message({{"id", 42}, {"type", "private"}}, {{"text", "/start abc123"}}), kBot, "8001");
    ASSERT_TRUE(parsed.inbound);
    EXPECT_EQ(parsed.update_id, 7);
    const auto& in = *parsed.inbound;
    EXPECT_EQ(in.address.kind, ChatKind::Private);
    EXPECT_EQ(in.address.chat, "42");
    EXPECT_EQ(in.address.sender, "42");
    EXPECT_TRUE(in.mentioned);
    EXPECT_EQ(in.start_code, "abc123");
    EXPECT_EQ(in.reply_context.value("message_id", 0), 55);
}

// 场景:群消息开头是 emoji(占 2 个 UTF-16 单位),后面 @AceTestBot。
// 期望:按 UTF-16 偏移正确找到 mention 并判定为点名;正文里去掉 @机器人。
TEST(TelegramUpdate, GroupMentionWithUtf16Offsets) {
    const std::string text = u8"😀 @AceTestBot 帮我看看";
    const auto parsed = parse_update(
        message(group_chat(), {{"text", text},
                               {"entities", nlohmann::json::array({{{"type", "mention"}, {"offset", 3}, {"length", 11}}})}}),
        kBot, "8001");
    ASSERT_TRUE(parsed.inbound);
    EXPECT_TRUE(parsed.inbound->mentioned);
    EXPECT_EQ(parsed.inbound->address.kind, ChatKind::Group);
    EXPECT_EQ(parsed.inbound->address.chat, "-1001");
    EXPECT_EQ(parsed.inbound->address.sender, "42");
    EXPECT_EQ(parsed.inbound->text, u8"😀  帮我看看");
}

// 场景:群里发 "/status@AceTestBot";另一条 @ 的是别的机器人;第三条没有任何点名。
// 期望:第一条算点名且命令变成 "/status";后两条都不算点名(核心会忽略)。
TEST(TelegramUpdate, CommandsForThisBotCountOthersDoNot) {
    const auto command = parse_update(
        message(group_chat(), {{"text", "/status@AceTestBot"},
                               {"entities", nlohmann::json::array({{{"type", "bot_command"}, {"offset", 0}, {"length", 18}}})}}),
        kBot, "8001");
    ASSERT_TRUE(command.inbound);
    EXPECT_TRUE(command.inbound->mentioned);
    EXPECT_EQ(command.inbound->text, "/status");
    const auto other = parse_update(
        message(group_chat(), {{"text", "@OtherBot hi"},
                               {"entities", nlohmann::json::array({{{"type", "mention"}, {"offset", 0}, {"length", 9}}})}}),
        kBot, "8001");
    ASSERT_TRUE(other.inbound);
    EXPECT_FALSE(other.inbound->mentioned);
    const auto plain = parse_update(message(group_chat(), {{"text", "just chatting"}}), kBot, "8001");
    ASSERT_TRUE(plain.inbound);
    EXPECT_FALSE(plain.inbound->mentioned);
}

// 场景:群里直接回复机器人之前的一条消息,话题群里带 message_thread_id。
// 期望:回复机器人算点名;记下被引用的文字与话题 id(话题只用于回复定位)。
TEST(TelegramUpdate, ReplyToBotCountsAsMentionAndKeepsThread) {
    const auto parsed = parse_update(
        message(group_chat(), {{"text", "继续"},
                               {"is_topic_message", true},
                               {"message_thread_id", 9},
                               {"reply_to_message", {{"message_id", 50}, {"from", {{"id", 8001}, {"is_bot", true}}},
                                                     {"text", "上一条回答"}}}}),
        kBot, "8001");
    ASSERT_TRUE(parsed.inbound);
    EXPECT_TRUE(parsed.inbound->mentioned);
    EXPECT_EQ(parsed.inbound->address.thread, "9");
    EXPECT_EQ(parsed.inbound->quote_text, u8"上一条回答");
    EXPECT_EQ(parsed.inbound->reply_context.value("thread", ""), "9");
}

// 场景:带说明文字的照片(多个尺寸)、图片类型的文件、语音、贴纸。
// 期望:照片取上限内最大的尺寸并用 caption 作正文;图片文件识别为图片;
// 语音与贴纸标为对应类型(核心会回复“暂不支持”)。
TEST(TelegramUpdate, ParsesMediaAttachments) {
    const auto photo = parse_update(
        message({{"id", 42}, {"type", "private"}},
                {{"caption", "看这张"},
                 {"photo", nlohmann::json::array({{{"file_id", "small"}, {"file_size", 100}},
                                                  {{"file_id", "large"}, {"file_size", 2000}}})}}),
        kBot, "8001");
    ASSERT_TRUE(photo.inbound);
    EXPECT_EQ(photo.inbound->text, u8"看这张");
    ASSERT_EQ(photo.inbound->attachments.size(), 1u);
    EXPECT_EQ(photo.inbound->attachments[0].kind, AttachmentKind::Image);
    EXPECT_EQ(photo.inbound->attachments[0].remote_ref, "large");

    const auto doc = parse_update(
        message({{"id", 42}, {"type", "private"}},
                {{"document", {{"file_id", "d1"}, {"file_name", "a.png"}, {"mime_type", "image/png"}, {"file_size", 10}}}}),
        kBot, "8001");
    ASSERT_TRUE(doc.inbound);
    EXPECT_EQ(doc.inbound->attachments.at(0).kind, AttachmentKind::Image);
    EXPECT_EQ(doc.inbound->attachments.at(0).name, "a.png");

    const auto voice = parse_update(message({{"id", 42}, {"type", "private"}}, {{"voice", {{"file_id", "v1"}}}}), kBot, "8001");
    ASSERT_TRUE(voice.inbound);
    EXPECT_EQ(voice.inbound->attachments.at(0).kind, AttachmentKind::Voice);
    const auto sticker = parse_update(message({{"id", 42}, {"type", "private"}}, {{"sticker", {{"file_id", "s1"}}}}), kBot, "8001");
    ASSERT_TRUE(sticker.inbound);
    EXPECT_EQ(sticker.inbound->attachments.at(0).kind, AttachmentKind::Sticker);
}

// 场景:频道帖子、其它机器人发的消息、非 message 更新。
// 期望:都不产生入站消息,但 update_id 照常返回,用于推进 offset。
TEST(TelegramUpdate, IgnoresChannelsBotsAndOtherUpdates) {
    const auto channel = parse_update(message({{"id", -1009}, {"type", "channel"}}, {{"text", "post"}}), kBot, "8001");
    EXPECT_FALSE(channel.inbound);
    EXPECT_EQ(channel.update_id, 7);
    auto from_bot = message({{"id", 42}, {"type", "private"}}, {{"text", "hi"}});
    from_bot["message"]["from"]["is_bot"] = true;
    EXPECT_FALSE(parse_update(from_bot, kBot, "8001").inbound);
    const auto edited = parse_update({{"update_id", 8}, {"edited_message", {{"text", "x"}}}}, kBot, "8001");
    EXPECT_FALSE(edited.inbound);
    EXPECT_EQ(edited.update_id, 8);
}

// 场景:按 UTF-16 偏移切含 emoji 与中文的文本,包括越界的长度。
// 期望:切出的子串与按字符直觉一致;越界时截到末尾,不切断字符。
TEST(TelegramUpdate, Utf16SliceHandlesSurrogatePairs) {
    const std::string text = u8"a😀中b";
    EXPECT_EQ(utf16_slice(text, 1, 2), u8"😀");
    EXPECT_EQ(utf16_slice(text, 3, 1), u8"中");
    EXPECT_EQ(utf16_slice(text, 3, 100), u8"中b");
}

} // namespace
} // namespace acecode::im::telegram
