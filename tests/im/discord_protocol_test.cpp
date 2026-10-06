#include <gtest/gtest.h>

#include "im/discord/discord_protocol.hpp"

// im/discord/discord_protocol:MESSAGE_CREATE 解析(私信 / 服务器频道 / 点名 / 过滤)、提及处理、
// 附件分类、错误响应解析与分类、邀请链接、Markdown 适配、URL 小工具。全部纯逻辑。

namespace acecode::im::discord {
namespace {

const std::string kBot = "900000000000000001";
const std::string kUser = "100000000000000007";
const std::string kChannel = "500000000000000001";
const std::string kGuild = "700000000000000001";

nlohmann::json dm_message(const std::string& content) {
    return {{"id", "600000000000000001"},
            {"channel_id", "8" + kUser},
            {"channel_type", 1},
            {"type", 0},
            {"author", {{"id", kUser}, {"username", "alice"}, {"global_name", "Alice"}}},
            {"content", content},
            {"mentions", nlohmann::json::array()},
            {"attachments", nlohmann::json::array()}};
}

nlohmann::json guild_message(const std::string& content) {
    return {{"id", "600000000000000002"},
            {"channel_id", kChannel},
            {"guild_id", kGuild},
            {"channel_type", 0},
            {"type", 0},
            {"author", {{"id", kUser}, {"username", "bob"}, {"global_name", "Bob"}}},
            {"member", {{"nick", "Bobby"}, {"roles", nlohmann::json::array()}}},
            {"content", content},
            {"mentions", nlohmann::json::array()},
            {"mention_roles", nlohmann::json::array()},
            {"attachments", nlohmann::json::array()}};
}

// 场景:用户私信机器人一句话(没有 guild_id,channel_type=1)。
// 期望:私聊地址的 chat 与 sender 都是对方用户 id、account 是机器人 id;私聊恒为点名;
// 回复上下文带消息 id 与私聊频道 id(没有 guild_id);同时给出“用户 → 私聊频道”供发送复用。
TEST(DiscordProtocol, ParsesDirectMessage) {
    const auto parsed = parse_message_create(dm_message("  你好  "), kBot, {});
    ASSERT_TRUE(parsed.inbound) << parsed.drop_reason;
    const auto& in = *parsed.inbound;
    EXPECT_EQ(in.address.platform, "discord");
    EXPECT_EQ(in.address.account, kBot);
    EXPECT_EQ(in.address.kind, ChatKind::Private);
    EXPECT_EQ(in.address.chat, kUser);
    EXPECT_EQ(in.address.sender, kUser);
    EXPECT_TRUE(in.mentioned);
    EXPECT_EQ(in.text, u8"你好");
    EXPECT_EQ(in.sender_name, "Alice");
    EXPECT_EQ(in.reply_context.value("message_id", ""), "600000000000000001");
    EXPECT_EQ(in.reply_context.value("channel_id", ""), "8" + kUser);
    EXPECT_FALSE(in.reply_context.contains("guild_id"));
    EXPECT_EQ(parsed.dm_user, kUser);
    EXPECT_EQ(parsed.dm_channel, "8" + kUser);
}

// 场景:旧载荷没有 channel_type 字段,也没有 guild_id。
// 期望:仍按私聊处理(以 guild_id 缺失为准)。
TEST(DiscordProtocol, MissingChannelTypeFallsBackToGuildIdCheck) {
    auto d = dm_message("hi");
    d.erase("channel_type");
    const auto parsed = parse_message_create(d, kBot, {});
    ASSERT_TRUE(parsed.inbound);
    EXPECT_EQ(parsed.inbound->address.kind, ChatKind::Private);
}

// 场景:服务器频道里 "<@机器人> 看看 <@!机器人> 这个 <@另一个人>",mentions 里有机器人与另一个人。
// 期望:群地址的 chat 是频道 id、sender 是发言人;视为点名;正文去掉机器人的两种提及写法,
// 不留双空格,其他人的提及换成 "@显示名";展示名优先用服务器昵称;回复上下文带 guild_id。
TEST(DiscordProtocol, ParsesGuildMentionAndStripsBotTokens) {
    auto d = guild_message("<@" + kBot + "> 看看 <@!" + kBot + "> 这个 <@200000000000000002>");
    d["mentions"] = nlohmann::json::array(
        {{{"id", kBot}, {"username", "AceBot"}, {"bot", true}},
         {{"id", "200000000000000002"}, {"username", "carol"}, {"global_name", "Carol"}}});
    const auto parsed = parse_message_create(d, kBot, {});
    ASSERT_TRUE(parsed.inbound) << parsed.drop_reason;
    const auto& in = *parsed.inbound;
    EXPECT_EQ(in.address.kind, ChatKind::Group);
    EXPECT_EQ(in.address.chat, kChannel);
    EXPECT_EQ(in.address.sender, kUser);
    EXPECT_TRUE(in.mentioned);
    EXPECT_EQ(in.text, u8"看看 这个 @Carol");
    EXPECT_EQ(in.sender_name, "Bobby");
    EXPECT_EQ(in.reply_context.value("guild_id", ""), kGuild);
    EXPECT_TRUE(parsed.dm_user.empty());
}

// 场景:手机端/转发的消息正文里有 <@机器人>,但 mentions 数组是空的。
// 期望:只看正文也能认出点名(hermes 同样两处都查)。
TEST(DiscordProtocol, InlineMentionWithoutMentionsArrayCounts) {
    const auto parsed = parse_message_create(guild_message("<@" + kBot + "> ping"), kBot, {});
    ASSERT_TRUE(parsed.inbound);
    EXPECT_TRUE(parsed.inbound->mentioned);
    EXPECT_EQ(parsed.inbound->text, "ping");
}

// 场景:服务器频道里一条没有点名机器人的普通消息。
// 期望:照常解析,但 mentioned=false(由核心忽略,传输层不替核心做决定)。
TEST(DiscordProtocol, GuildMessageWithoutMentionIsNotMentioned) {
    const auto parsed = parse_message_create(guild_message("大家好"), kBot, {});
    ASSERT_TRUE(parsed.inbound);
    EXPECT_FALSE(parsed.inbound->mentioned);
    EXPECT_EQ(parsed.inbound->text, u8"大家好");
}

// 场景:用户回复机器人的消息(type 19),并且关掉了回复里的 @(mentions 里没有机器人)。
// 期望:看被回复消息的作者是机器人,算点名;被回复消息的正文作为 quote_text。
TEST(DiscordProtocol, ReplyToBotCountsAsMention) {
    auto d = guild_message("继续说");
    d["type"] = 19;
    d["message_reference"] = {{"message_id", "600000000000000000"}};
    d["referenced_message"] = {{"id", "600000000000000000"},
                               {"author", {{"id", kBot}, {"username", "AceBot"}, {"bot", true}}},
                               {"content", "上一条回答"}};
    const auto parsed = parse_message_create(d, kBot, {});
    ASSERT_TRUE(parsed.inbound);
    EXPECT_TRUE(parsed.inbound->mentioned);
    EXPECT_EQ(parsed.inbound->quote_text, u8"上一条回答");
}

// 场景:用户在补全里选了机器人的托管角色,正文是 <@&角色 id>,mention_roles 里也有它。
// 期望:已知机器人角色时算点名,角色提及从正文里去掉;mention_everyone 不算点名。
TEST(DiscordProtocol, BotManagedRoleMentionCounts) {
    const std::string role = "300000000000000003";
    auto d = guild_message("<@&" + role + "> 帮忙");
    d["mention_roles"] = nlohmann::json::array({role});
    const auto parsed = parse_message_create(d, kBot, {role});
    ASSERT_TRUE(parsed.inbound);
    EXPECT_TRUE(parsed.inbound->mentioned);
    EXPECT_EQ(parsed.inbound->text, u8"帮忙");

    auto everyone = guild_message("@everyone 开会");
    everyone["mention_everyone"] = true;
    const auto plain = parse_message_create(everyone, kBot, {role});
    ASSERT_TRUE(plain.inbound);
    EXPECT_FALSE(plain.inbound->mentioned);
}

// 场景:GUILD_CREATE 的角色列表里有一个 tags.bot_id 等于机器人 id 的托管角色。
// 期望:找出这个角色 id;其他角色不算。
TEST(DiscordProtocol, FindsBotManagedRoleInGuildCreate) {
    const nlohmann::json guild{{"id", kGuild},
                               {"roles", nlohmann::json::array(
                                             {{{"id", "300000000000000001"}, {"name", "everyone"}},
                                              {{"id", "300000000000000003"}, {"tags", {{"bot_id", kBot}}}},
                                              {{"id", "300000000000000004"}, {"tags", {{"bot_id", "1"}}}}})}};
    const auto roles = bot_role_ids_in_guild(guild, kBot);
    ASSERT_EQ(roles.size(), 1u);
    EXPECT_EQ(roles[0], "300000000000000003");
}

// 场景:自己发出的消息回显、其他机器人、webhook、系统消息类型(置顶 6、子区首条 21)、群组私聊、
// 只 @ 了机器人没有任何内容。
// 期望:全部丢弃并给出原因(日志用);不会产生入站消息。
TEST(DiscordProtocol, DropsSelfBotsSystemTypesAndBareMentions) {
    auto self = guild_message("echo");
    self["author"]["id"] = kBot;
    auto bot = guild_message("hi");
    bot["author"]["bot"] = true;
    auto webhook = guild_message("hook");
    webhook["webhook_id"] = "123456789012345678";
    auto pin = guild_message("");
    pin["type"] = 6;
    auto starter = guild_message("thread starter");
    starter["type"] = 21;
    auto group_dm = dm_message("hi");
    group_dm["channel_type"] = 3;
    const auto bare = guild_message("<@" + kBot + ">");
    for (const auto& d : {self, bot, webhook, pin, starter, group_dm, bare}) {
        const auto parsed = parse_message_create(d, kBot, {});
        EXPECT_FALSE(parsed.inbound) << d.dump();
        EXPECT_FALSE(parsed.drop_reason.empty());
    }
}

// 场景:消息带三个附件(带 charset 的文本、图片、视频),另一条是语音消息(flags 含 1<<13)。
// 期望:mime 去掉参数并转小写;按前缀分为文件 / 图片 / 视频;语音消息的附件标为语音;
// remote_ref 是 CDN 地址、size 原样带上;只有附件没有正文也算有效消息。
TEST(DiscordProtocol, ClassifiesAttachments) {
    auto d = dm_message("");
    d["attachments"] = nlohmann::json::array(
        {{{"id", "1"}, {"filename", "log.txt"}, {"size", 1234}, {"content_type", "Text/Plain; charset=utf-8"},
          {"url", "https://cdn.discordapp.com/attachments/5/1/log.txt?ex=a&is=b&hm=c&"}},
         {{"id", "2"}, {"filename", "a.png"}, {"size", 10}, {"content_type", "image/png"}, {"url", "https://cdn/x/a.png"}},
         {{"id", "3"}, {"filename", "v.mp4"}, {"size", 10}, {"content_type", "video/mp4"}, {"url", "https://cdn/x/v.mp4"}}});
    const auto parsed = parse_message_create(d, kBot, {});
    ASSERT_TRUE(parsed.inbound);
    const auto& list = parsed.inbound->attachments;
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0].kind, AttachmentKind::File);
    EXPECT_EQ(list[0].mime_type, "text/plain");
    EXPECT_EQ(list[0].size, 1234u);
    EXPECT_EQ(list[0].name, "log.txt");
    EXPECT_EQ(list[0].remote_ref, "https://cdn.discordapp.com/attachments/5/1/log.txt?ex=a&is=b&hm=c&");
    EXPECT_EQ(list[1].kind, AttachmentKind::Image);
    EXPECT_EQ(list[2].kind, AttachmentKind::Video);

    auto voice = dm_message("");
    voice["flags"] = 1 << 13;
    voice["attachments"] = nlohmann::json::array(
        {{{"id", "4"}, {"filename", "voice-message.ogg"}, {"content_type", "audio/ogg"}, {"url", "https://cdn/v.ogg"}}});
    const auto voiced = parse_message_create(voice, kBot, {});
    ASSERT_TRUE(voiced.inbound);
    EXPECT_EQ(voiced.inbound->attachments.at(0).kind, AttachmentKind::Voice);
}

// 场景:用户转发了一条带文件的消息:外层正文为空,内容在 message_snapshots[0].message 里。
// 期望:用快照的正文作为 text,快照的附件也收进来。
TEST(DiscordProtocol, UsesForwardedSnapshotContent) {
    auto d = dm_message("");
    d["message_snapshots"] = nlohmann::json::array(
        {{{"message", {{"content", "转发的内容"},
                       {"attachments", nlohmann::json::array({{{"id", "9"}, {"filename", "f.pdf"},
                                                               {"content_type", "application/pdf"},
                                                               {"url", "https://cdn/f.pdf"}}})}}}}});
    const auto parsed = parse_message_create(d, kBot, {});
    ASSERT_TRUE(parsed.inbound);
    EXPECT_EQ(parsed.inbound->text, u8"转发的内容");
    ASSERT_EQ(parsed.inbound->attachments.size(), 1u);
    EXPECT_EQ(parsed.inbound->attachments[0].name, "f.pdf");
}

// 场景:发消息失败的几种响应体:回复定位字段校验失败(50035 嵌套 errors)、被回复消息不存在(10008)、
// 没有读历史权限(160002)、缺权限(50013)、429 限流体、413 / 40005 文件过大、非 JSON 的 502。
// 期望:50035 的 detail 带出字段路径与原因;前三种判定为“去掉回复定位重发即可”,50013 不是;
// 429 读出小数秒 retry_after 与 global;过大判定正确;非 JSON 用 "HTTP 状态码" 兜底;
// 给用户的中文原因能区分私信被拒与缺权限。
TEST(DiscordProtocol, ParsesAndClassifiesApiErrors) {
    const auto form = parse_api_error(
        400, R"({"code":50035,"message":"Invalid Form Body","errors":{"message_reference":{"_errors":[{"code":"MESSAGE_REFERENCE_UNKNOWN_MESSAGE","message":"Unknown message"}]}}})");
    EXPECT_EQ(form.code, 50035);
    EXPECT_NE(form.detail.find("message_reference: Unknown message"), std::string::npos) << form.detail;
    EXPECT_TRUE(reference_rejected(form));
    EXPECT_TRUE(reference_rejected(parse_api_error(404, R"({"code":10008,"message":"Unknown Message"})")));
    EXPECT_TRUE(reference_rejected(parse_api_error(403, R"({"code":160002,"message":"Cannot reply without permission to read message history"})")));
    const auto perms = parse_api_error(403, R"({"code":50013,"message":"Missing Permissions"})");
    EXPECT_FALSE(reference_rejected(perms));
    EXPECT_NE(describe_error(perms).find(u8"权限"), std::string::npos);
    EXPECT_NE(describe_error(parse_api_error(403, R"({"code":50007,"message":"Cannot send messages to this user"})")).find(u8"私信"),
              std::string::npos);

    const auto limited = parse_api_error(429, R"({"message":"You are being rate limited.","retry_after":64.57,"global":true,"code":20028})");
    EXPECT_DOUBLE_EQ(limited.retry_after, 64.57);
    EXPECT_TRUE(limited.global);

    EXPECT_TRUE(upload_too_large(parse_api_error(413, "")));
    EXPECT_TRUE(upload_too_large(parse_api_error(400, R"({"code":40005,"message":"Request entity too large"})")));
    const auto html = parse_api_error(502, "<html>bad gateway</html>");
    EXPECT_EQ(html.code, 0);
    EXPECT_EQ(html.message, "HTTP 502");
}

// 场景:由应用 id 生成邀请链接;由 /applications/@me 的 flags 判断 Message Content Intent。
// 期望:链接 scope=bot、权限值 274878008320(查看频道 + 发送消息 + 附加文件 + 读历史 + 子区发送);
// 空 id 不生成链接;1<<18 或 1<<19 任一置位即已开启,都没有为未开启,没有 flags 为未知。
TEST(DiscordProtocol, BuildsInviteUrlAndReadsIntentFlags) {
    EXPECT_EQ(invite_url("900000000000000001"),
              "https://discord.com/oauth2/authorize?client_id=900000000000000001&scope=bot"
              "&permissions=274878008320&integration_type=0");
    EXPECT_TRUE(invite_url("").empty());
    EXPECT_EQ(message_content_intent({{"flags", 1 << 18}}), IntentState::Enabled);
    EXPECT_EQ(message_content_intent({{"flags", (1 << 19) | 1}}), IntentState::Enabled);
    EXPECT_EQ(message_content_intent({{"flags", 0}}), IntentState::Disabled);
    EXPECT_EQ(message_content_intent(nlohmann::json::object()), IntentState::Unknown);
}

// 场景:/gateway/bot 给出的地址可能带或不带结尾斜杠,恢复地址同理。
// 期望:统一拼成 "<地址>/?v=10&encoding=json"(不带 compress);原有查询串被替换。
TEST(DiscordProtocol, BuildsGatewayConnectUrl) {
    EXPECT_EQ(gateway_connect_url("wss://gateway.discord.gg"), "wss://gateway.discord.gg/?v=10&encoding=json");
    EXPECT_EQ(gateway_connect_url("wss://gateway-us-east1-b.discord.gg/"),
              "wss://gateway-us-east1-b.discord.gg/?v=10&encoding=json");
    EXPECT_EQ(gateway_connect_url("wss://gateway.discord.gg/?v=9"), "wss://gateway.discord.gg/?v=10&encoding=json");
    EXPECT_TRUE(gateway_connect_url("").empty());
}

// 场景:助手回复里有 GFM 表格、四级标题,以及代码块里长得像表格/标题的内容。
// 期望:表格整体包进 ``` 代码块(Discord 不渲染表格);#### 标题改成粗体;代码块内原样保留;
// 普通 ### 标题(Discord 支持)不动。
TEST(DiscordProtocol, FormatsTablesAndDeepHeadings) {
    const std::string input = "### 结果\n| 名称 | 值 |\n|---|---|\n| a | 1 |\n\n#### 细节\n```\n| x | y |\n|---|---|\n#### keep\n```";
    const std::string expected = "### 结果\n```\n| 名称 | 值 |\n|---|---|\n| a | 1 |\n```\n\n**细节**\n```\n| x | y |\n|---|---|\n#### keep\n```";
    EXPECT_EQ(format_markdown(input), expected);
    EXPECT_EQ(format_markdown("普通 **文字** a|b"), "普通 **文字** a|b");
}

// 场景:取附件地址的主机名与附件 id;User-Agent 的格式。
// 期望:主机名小写、去掉端口与用户信息;从 /attachments/<频道>/<附件>/ 取出附件 id;
// UA 形如 "DiscordBot (<项目地址>, <版本>)"(Discord 要求的格式)。
TEST(DiscordProtocol, UrlHelpersAndUserAgent) {
    EXPECT_EQ(url_host("https://CDN.DiscordApp.com/attachments/1/2/a.txt?ex=1"), "cdn.discordapp.com");
    EXPECT_EQ(url_host("http://user@127.0.0.1:8080/x"), "127.0.0.1");
    EXPECT_EQ(url_host("not a url"), "");
    EXPECT_EQ(attachment_id_from_url("https://cdn.discordapp.com/attachments/500/1290000000000000099/log.txt?ex=1"),
              "1290000000000000099");
    EXPECT_EQ(attachment_id_from_url("https://example.com/files/1/2"), "");
    EXPECT_EQ(discord_user_agent().rfind("DiscordBot (https://github.com/tmoonlight/acecode, ", 0), 0u);
    EXPECT_EQ(discord_user_agent().back(), ')');
}

} // namespace
} // namespace acecode::im::discord
