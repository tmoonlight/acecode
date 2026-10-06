#include <gtest/gtest.h>

#include "im/line/line_media.hpp"
#include "im/line/line_protocol.hpp"

#include <nlohmann/json.hpp>

#include <string>

// im/line/line_protocol 与 line_media:LINE 纯逻辑单测。
// 覆盖:签名(官方示例)、webhook 解析(私聊 / 群 @ / @All / 电脑版无 userId / 各类附件 / 位置 / standby /
// 重投 / 无效 JSON)、UTF-16 区间删除、回复令牌新鲜度、分段与每次 5 条、错误识别、链接与文件名、媒体登记表。

namespace acecode::im::line {
namespace {

constexpr const char* kBot = "U0123456789abcdef0123456789abcdef";
constexpr const char* kUser = "U11111111111111111111111111111111";
constexpr const char* kSecret = "8c570fa6dd201bb328f1c1eac23a96d8";
constexpr std::int64_t kNow = 1700000000000;

std::string body_with(const nlohmann::json& event) {
    return nlohmann::json{{"destination", kBot}, {"events", nlohmann::json::array({event})}}.dump();
}

nlohmann::json message_event(const nlohmann::json& source, const nlohmann::json& message) {
    return {{"type", "message"}, {"mode", "active"}, {"timestamp", kNow - 1000},
            {"source", source}, {"webhookEventId", "01FZ74A0TDDPYRVKNK77XKC3ZR"},
            {"deliveryContext", {{"isRedelivery", false}}}, {"replyToken", "rt-1"}, {"message", message}};
}

// 场景:LINE 官方文档给出的签名示例(secret、请求体、期望签名)。
// 期望:按原始字节计算的 base64(HMAC-SHA256) 与官方值一致,校验通过。
TEST(LineProtocol, SignatureMatchesOfficialVector) {
    const std::string body = R"({"destination":"U8e742f61d673b39c7fff3cecb7536ef0","events":[]})";
    EXPECT_EQ(sign_body(body, kSecret), "GhRKmvmHys4Pi8DxkF4+EayaH0OqtJtaZxgTD9fMDLs=");
    EXPECT_TRUE(verify_signature(body, "GhRKmvmHys4Pi8DxkF4+EayaH0OqtJtaZxgTD9fMDLs=", kSecret));
    // 头值两端的空白不影响校验(代理有时会带上)。
    EXPECT_TRUE(verify_signature(body, " GhRKmvmHys4Pi8DxkF4+EayaH0OqtJtaZxgTD9fMDLs=\r\n", kSecret));
}

// 场景:请求体被改动一个字节(如代理把 LF 换成 CRLF)、签名缺失、密钥不对或为空。
// 期望:全部拒绝 —— 签名必须按原样字节、正确密钥计算。
TEST(LineProtocol, SignatureRejectsTamperingAndMissingValues) {
    const std::string body = R"({"destination":"U8e742f61d673b39c7fff3cecb7536ef0","events":[]})";
    const auto good = sign_body(body, kSecret);
    EXPECT_FALSE(verify_signature(body + "\n", good, kSecret));
    EXPECT_FALSE(verify_signature(body, "", kSecret));
    EXPECT_FALSE(verify_signature(body, good, "00000000000000000000000000000000"));
    EXPECT_FALSE(verify_signature(body, good, ""));
    EXPECT_FALSE(verify_signature(body, good.substr(0, good.size() - 1), kSecret));
}

// 场景:用户一对一发来一条文字。
// 期望:私聊地址 chat = sender = userId,account = 机器人 id,视为点名;reply_context 带回复令牌、
// 本机收到时间、事件时间、引用令牌与消息 id。
TEST(LineProtocol, ParsesPrivateText) {
    const auto body = body_with(message_event({{"type", "user"}, {"userId", kUser}},
                                              {{"id", "100"}, {"type", "text"}, {"text", " 你好 "}, {"quoteToken", "qt"}}));
    const auto parsed = parse_webhook(body, kBot, kNow);
    ASSERT_TRUE(parsed.valid);
    EXPECT_EQ(parsed.destination, kBot);
    ASSERT_EQ(parsed.events.size(), 1u);
    const auto& event = parsed.events[0];
    EXPECT_EQ(event.kind, EventKind::Message);
    EXPECT_EQ(event.event_id, "01FZ74A0TDDPYRVKNK77XKC3ZR");
    ASSERT_TRUE(event.inbound.has_value());
    const auto& in = *event.inbound;
    EXPECT_EQ(in.address.platform, "line");
    EXPECT_EQ(in.address.account, kBot);
    EXPECT_EQ(in.address.kind, ChatKind::Private);
    EXPECT_EQ(in.address.chat, kUser);
    EXPECT_EQ(in.address.sender, kUser);
    EXPECT_EQ(in.message_id, "100");
    EXPECT_EQ(in.text, u8"你好");
    EXPECT_TRUE(in.mentioned);
    EXPECT_EQ(in.reply_context.value("reply_token", ""), "rt-1");
    EXPECT_EQ(in.reply_context.value("received_at_ms", std::int64_t{0}), kNow);
    EXPECT_EQ(in.reply_context.value("event_ts_ms", std::int64_t{0}), kNow - 1000);
    EXPECT_EQ(in.reply_context.value("quote_token", ""), "qt");
    EXPECT_EQ(in.reply_context.value("message_id", ""), "100");
}

// 场景:群里有人发 “😀@ACE 帮我看看”,mention 的 index/length 按 UTF-16 计(emoji 占 2 个单位)。
// 期望:按 UTF-16 区间去掉 @ACE,不切坏 emoji;只因 isSelf 点名机器人才 mentioned;群地址 chat = groupId。
// 回归:若按 UTF-8 字节切片,emoji 会被截成乱码、“@ACE” 残留一部分。
TEST(LineProtocol, GroupSelfMentionIsStrippedByUtf16Offsets) {
    const std::string text = u8"😀@ACE 帮我看看";
    const nlohmann::json mention{{"mentionees", {{{"index", 2}, {"length", 4}, {"type", "user"},
                                                  {"userId", kBot}, {"isSelf", true}}}}};
    const auto body = body_with(message_event({{"type", "group"}, {"groupId", "Cgroup1"}, {"userId", kUser}},
                                              {{"id", "101"}, {"type", "text"}, {"text", text},
                                               {"quoteToken", "q"}, {"mention", mention}}));
    const auto parsed = parse_webhook(body, kBot, kNow);
    ASSERT_EQ(parsed.events.size(), 1u);
    ASSERT_TRUE(parsed.events[0].inbound.has_value());
    const auto& in = *parsed.events[0].inbound;
    EXPECT_EQ(in.address.kind, ChatKind::Group);
    EXPECT_EQ(in.address.chat, "Cgroup1");
    EXPECT_EQ(in.address.sender, kUser);
    EXPECT_TRUE(in.mentioned);
    EXPECT_EQ(in.text, u8"😀 帮我看看");
}

// 场景:群消息只 @All,或点名了别人;另一条没有 isSelf 字段但 userId 等于机器人 id。
// 期望:@All 与点名别人都不算点名机器人,文字原样保留;userId 兜底判定为点名。
TEST(LineProtocol, MentionRulesForAllOthersAndFallback) {
    const nlohmann::json all{{"mentionees", {{{"index", 0}, {"length", 4}, {"type", "all"}},
                                             {{"index", 5}, {"length", 4}, {"type", "user"}, {"userId", kUser}}}}};
    auto parsed = parse_webhook(body_with(message_event({{"type", "group"}, {"groupId", "Cg"}, {"userId", kUser}},
                                                        {{"id", "1"}, {"type", "text"}, {"text", "@All @Bob hi"},
                                                         {"quoteToken", "q"}, {"mention", all}})),
                                kBot, kNow);
    ASSERT_TRUE(parsed.events[0].inbound.has_value());
    EXPECT_FALSE(parsed.events[0].inbound->mentioned);
    EXPECT_EQ(parsed.events[0].inbound->text, "@All @Bob hi");

    const nlohmann::json fallback{{"mentionees", {{{"index", 0}, {"length", 4}, {"type", "user"}, {"userId", kBot}}}}};
    parsed = parse_webhook(body_with(message_event({{"type", "room"}, {"roomId", "Rroom"}, {"userId", kUser}},
                                                   {{"id", "2"}, {"type", "text"}, {"text", "@ACE hi"},
                                                    {"quoteToken", "q"}, {"mention", fallback}})),
                           kBot, kNow);
    ASSERT_TRUE(parsed.events[0].inbound.has_value());
    EXPECT_TRUE(parsed.events[0].inbound->mentioned);
    EXPECT_EQ(parsed.events[0].inbound->address.chat, "Rroom");
    EXPECT_EQ(parsed.events[0].inbound->text, "hi");
}

// 场景:电脑版 LINE 用户在群里 @机器人,事件 source 没有 userId。
// 期望:不生成入站消息(无法识别发言人,不能做成员授权),但标出 unidentified_sender 与 mentioned_self,
// 传输层据此用免费回复令牌提示对方。
TEST(LineProtocol, GroupMessageWithoutUserIdIsUnidentified) {
    const nlohmann::json mention{{"mentionees", {{{"index", 0}, {"length", 4}, {"type", "user"}, {"isSelf", true}}}}};
    const auto parsed = parse_webhook(body_with(message_event({{"type", "group"}, {"groupId", "Cg"}},
                                                              {{"id", "3"}, {"type", "text"}, {"text", "@ACE hi"},
                                                               {"quoteToken", "q"}, {"mention", mention}})),
                                      kBot, kNow);
    ASSERT_EQ(parsed.events.size(), 1u);
    EXPECT_FALSE(parsed.events[0].inbound.has_value());
    EXPECT_TRUE(parsed.events[0].unidentified_sender);
    EXPECT_TRUE(parsed.events[0].mentioned_self);
    EXPECT_EQ(parsed.events[0].reply_token, "rt-1");
}

// 场景:私聊依次发来图片、外部图片、视频、语音、文件、贴图、位置。
// 期望:图片/视频/语音的 remote_ref 是消息 id(外部内容取原始地址),文件带文件名与大小,
// 贴图是不可下载的 Sticker 附件,位置转成一行文字。
TEST(LineProtocol, MapsMediaAndLocationMessages) {
    const nlohmann::json src{{"type", "user"}, {"userId", kUser}};
    auto first = [&src](const nlohmann::json& message) {
        const auto parsed = parse_webhook(body_with(message_event(src, message)), kBot, kNow);
        EXPECT_EQ(parsed.events.size(), 1u);
        return parsed.events.at(0).inbound;
    };
    auto in = first({{"id", "200"}, {"type", "image"}, {"quoteToken", "q"}, {"contentProvider", {{"type", "line"}}}});
    ASSERT_TRUE(in && in->attachments.size() == 1);
    EXPECT_EQ(in->attachments[0].kind, AttachmentKind::Image);
    EXPECT_EQ(in->attachments[0].remote_ref, "200");
    EXPECT_EQ(in->attachments[0].mime_type, "image/jpeg");

    in = first({{"id", "201"}, {"type", "image"}, {"quoteToken", "q"},
                {"contentProvider", {{"type", "external"}, {"originalContentUrl", "https://cdn.example/a.jpg"}}}});
    ASSERT_TRUE(in && in->attachments.size() == 1);
    EXPECT_EQ(in->attachments[0].remote_ref, "https://cdn.example/a.jpg");

    in = first({{"id", "202"}, {"type", "video"}, {"quoteToken", "q"}, {"duration", 1000},
                {"contentProvider", {{"type", "line"}}}});
    ASSERT_TRUE(in && in->attachments.size() == 1);
    EXPECT_EQ(in->attachments[0].kind, AttachmentKind::Video);

    in = first({{"id", "203"}, {"type", "audio"}, {"duration", 1000}, {"contentProvider", {{"type", "line"}}}});
    ASSERT_TRUE(in && in->attachments.size() == 1);
    EXPECT_EQ(in->attachments[0].kind, AttachmentKind::Voice);
    EXPECT_EQ(in->attachments[0].name, "203.m4a");

    in = first({{"id", "204"}, {"type", "file"}, {"fileName", u8"报告.pdf"}, {"fileSize", 2048}});
    ASSERT_TRUE(in && in->attachments.size() == 1);
    EXPECT_EQ(in->attachments[0].kind, AttachmentKind::File);
    EXPECT_EQ(in->attachments[0].name, u8"报告.pdf");
    EXPECT_EQ(in->attachments[0].size, 2048u);
    EXPECT_EQ(in->attachments[0].remote_ref, "204");

    in = first({{"id", "205"}, {"type", "sticker"}, {"packageId", "1"}, {"stickerId", "2"},
                {"stickerResourceType", "STATIC"}, {"quoteToken", "q"}});
    ASSERT_TRUE(in && in->attachments.size() == 1);
    EXPECT_EQ(in->attachments[0].kind, AttachmentKind::Sticker);
    EXPECT_TRUE(in->attachments[0].remote_ref.empty());

    in = first({{"id", "206"}, {"type", "location"}, {"title", "Office"}, {"address", "Taipei"},
                {"latitude", 25.033}, {"longitude", 121.5654}});
    ASSERT_TRUE(in.has_value());
    EXPECT_EQ(in->text, u8"[位置] Office Taipei (25.033000, 121.565400)");
}

// 场景:standby 模式的事件(另一个模块接管了聊天)、重投事件、空事件列表、无效 JSON、未知事件与消息类型。
// 期望:standby 标记出来且 reply_context 不带令牌;重投标志进 reply_context;空列表与未知类型被容忍;
// 无效 JSON 判为 invalid。
TEST(LineProtocol, ToleratesStandbyRedeliveryAndUnknownShapes) {
    auto standby = message_event({{"type", "user"}, {"userId", kUser}},
                                 {{"id", "300"}, {"type", "text"}, {"text", "x"}, {"quoteToken", "q"}});
    standby["mode"] = "standby";
    standby.erase("replyToken");
    auto parsed = parse_webhook(body_with(standby), kBot, kNow);
    ASSERT_EQ(parsed.events.size(), 1u);
    EXPECT_TRUE(parsed.events[0].standby);
    ASSERT_TRUE(parsed.events[0].inbound.has_value());
    EXPECT_FALSE(parsed.events[0].inbound->reply_context.contains("reply_token"));

    auto redelivered = message_event({{"type", "user"}, {"userId", kUser}},
                                     {{"id", "301"}, {"type", "text"}, {"text", "x"}, {"quoteToken", "q"}});
    redelivered["deliveryContext"]["isRedelivery"] = true;
    parsed = parse_webhook(body_with(redelivered), kBot, kNow);
    ASSERT_TRUE(parsed.events[0].inbound.has_value());
    EXPECT_TRUE(parsed.events[0].redelivery);
    EXPECT_TRUE(parsed.events[0].inbound->reply_context.value("redelivery", false));

    parsed = parse_webhook(R"({"destination":"U1","events":[]})", kBot, kNow);
    EXPECT_TRUE(parsed.valid);
    EXPECT_TRUE(parsed.events.empty());

    parsed = parse_webhook("not json", kBot, kNow);
    EXPECT_FALSE(parsed.valid);

    parsed = parse_webhook(body_with({{"type", "brandNewEvent"}, {"webhookEventId", "E1"}}), kBot, kNow);
    ASSERT_EQ(parsed.events.size(), 1u);
    EXPECT_EQ(parsed.events[0].kind, EventKind::Other);
    EXPECT_FALSE(parsed.events[0].inbound.has_value());

    parsed = parse_webhook(body_with(message_event({{"type", "user"}, {"userId", kUser}},
                                                   {{"id", "302"}, {"type", "hologram"}})),
                           kBot, kNow);
    EXPECT_FALSE(parsed.events[0].inbound.has_value());

    parsed = parse_webhook(body_with({{"type", "unsend"}, {"webhookEventId", "E2"},
                                      {"source", {{"type", "user"}, {"userId", kUser}}},
                                      {"unsend", {{"messageId", "777"}}}}),
                           kBot, kNow);
    EXPECT_EQ(parsed.events[0].kind, EventKind::Unsend);
    EXPECT_EQ(parsed.events[0].message_id, "777");
}

// 场景:删除多个 UTF-16 区间:重叠、无序、越界、长度为 0;以及代理对中间的偏移。
// 期望:重叠部分只删一次,越界截到末尾,长度 0 忽略;偏移落在代理对中间时取该字符开头。
TEST(LineProtocol, RemovesUtf16RangesSafely) {
    EXPECT_EQ(remove_utf16_ranges("abcdef", {{4, 2}, {1, 2}, {2, 1}}), "ad");
    EXPECT_EQ(remove_utf16_ranges("abc", {{2, 10}}), "ab");
    EXPECT_EQ(remove_utf16_ranges("abc", {{1, 0}}), "abc");
    const std::string emoji = u8"a😀b";
    EXPECT_EQ(utf16_to_byte_offset(emoji, 1), 1u);
    EXPECT_EQ(utf16_to_byte_offset(emoji, 2), 1u);  // 代理对中间
    EXPECT_EQ(utf16_to_byte_offset(emoji, 3), 5u);
    EXPECT_EQ(utf16_to_byte_offset(emoji, 99), emoji.size());
}

// 场景:回复令牌的几种时间状态。阈值:收到后 50 秒内可用(官方说 1 分钟,留 10 秒余量);
// 重投事件在事件发生 20 分钟后一律失效(再留 1 分钟余量)。
// 期望:新鲜可用;超过 50 秒、缺令牌、缺收到时间不可用;重投且事件已过 19 分钟以上不可用。
TEST(LineProtocol, ReplyTokenFreshnessTable) {
    ReplyToken token;
    token.token = "rt";
    token.received_at_ms = kNow;
    token.event_ts_ms = kNow - 1000;
    EXPECT_TRUE(reply_token_fresh(token, kNow + 10'000));
    EXPECT_FALSE(reply_token_fresh(token, kNow + 50'000));
    auto missing = token;
    missing.token.clear();
    EXPECT_FALSE(reply_token_fresh(missing, kNow));
    auto no_time = token;
    no_time.received_at_ms = 0;
    EXPECT_FALSE(reply_token_fresh(no_time, kNow));
    auto redelivered = token;
    redelivered.redelivery = true;
    EXPECT_TRUE(reply_token_fresh(redelivered, kNow + 1000));
    redelivered.event_ts_ms = kNow - 19 * 60 * 1000 - 1;
    EXPECT_FALSE(reply_token_fresh(redelivered, kNow + 1000));

    const auto context = make_reply_context(token, "m1");
    const auto back = reply_token_of(context);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->token, "rt");
    EXPECT_EQ(back->received_at_ms, kNow);
    EXPECT_FALSE(reply_token_of(nlohmann::json::object()).has_value());
}

// 场景:助手回复是带链接与粗体的 Markdown,另有一段超长文字。
// 期望:转成纯文本且链接保留为 “文字 (网址)”;长文按 4800 个 UTF-16 单位分段;12 条消息分成 5/5/2 三次调用。
TEST(LineProtocol, ChunksPlainTextAndBatchesByFive) {
    const auto chunks = plain_text_chunks("**重点**:见 [文档](https://example.com/doc)");
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0], u8"重点:见 文档 (https://example.com/doc)");

    const auto long_chunks = plain_text_chunks(std::string(10000, 'a'));
    ASSERT_EQ(long_chunks.size(), 3u);
    for (const auto& chunk : long_chunks) EXPECT_LE(chunk.size(), kChunkUnits);

    std::vector<nlohmann::json> messages;
    for (int i = 0; i < 12; ++i) messages.push_back(text_message(std::to_string(i)));
    const auto batches = batch_messages(messages);
    ASSERT_EQ(batches.size(), 3u);
    EXPECT_EQ(batches[0].size(), 5u);
    EXPECT_EQ(batches[1].size(), 5u);
    EXPECT_EQ(batches[2].size(), 2u);
    EXPECT_EQ(batches[2][1]["text"], "11");
    EXPECT_EQ(text_message("x", "qt")["quoteToken"], "qt");
    EXPECT_FALSE(text_message("x").contains("quoteToken"));
}

// 场景:LINE 返回的几种错误。
// 期望:429 + “monthly limit” 是额度用完(暂存),普通 429 是限频;400 “Invalid reply token” 识别为令牌失效。
TEST(LineProtocol, ClassifiesQuotaAndReplyTokenErrors) {
    EXPECT_TRUE(is_monthly_limit(429, "You have reached your monthly limit."));
    EXPECT_FALSE(is_monthly_limit(429, "The API rate limit has been exceeded. Try again later."));
    EXPECT_FALSE(is_monthly_limit(400, "monthly limit"));
    EXPECT_TRUE(is_invalid_reply_token(400, "Invalid reply token"));
    EXPECT_FALSE(is_invalid_reply_token(400, "Failed to send messages"));
}

// 场景:机主用预填 “/start 483920” 的 oaMessage 链接私聊机器人;群里有人发同样的文字。
// 期望:链接按 UTF-8 百分号编码;私聊解析出 start_code,群聊不解析。
TEST(LineProtocol, StartCodeAndOaMessageLink) {
    EXPECT_EQ(oa_message_url("@216ruabc", "/start 483920"),
              "https://line.me/R/oaMessage/%40216ruabc/?%2Fstart%20483920");
    auto parsed = parse_webhook(body_with(message_event({{"type", "user"}, {"userId", kUser}},
                                                        {{"id", "400"}, {"type", "text"}, {"text", "/start 483920"},
                                                         {"quoteToken", "q"}})),
                                kBot, kNow);
    ASSERT_TRUE(parsed.events[0].inbound.has_value());
    EXPECT_EQ(parsed.events[0].inbound->start_code, "483920");
    parsed = parse_webhook(body_with(message_event({{"type", "group"}, {"groupId", "Cg"}, {"userId", kUser}},
                                                   {{"id", "401"}, {"type", "text"}, {"text", "/start 483920"},
                                                    {"quoteToken", "q"}})),
                           kBot, kNow);
    ASSERT_TRUE(parsed.events[0].inbound.has_value());
    EXPECT_TRUE(parsed.events[0].inbound->start_code.empty());
}

// 场景:生成加好友链接、判断可显示动画的 id、判断图片类型、清洗媒体文件名。
// 期望:basicId 的 @ 被编码;只有 U 开头的 id 能显示动画;只认 JPEG/PNG;
// 文件名只保留安全字符、不以点开头、不含路径分隔符,超长截断但保留扩展名。
TEST(LineProtocol, LinksIdsImagesAndFileNames) {
    EXPECT_EQ(add_friend_url("@216ruabc"), "https://line.me/R/ti/p/%40216ruabc");
    EXPECT_EQ(add_friend_url(""), "");
    EXPECT_TRUE(is_user_id(kUser));
    EXPECT_FALSE(is_user_id("Cgroup"));
    EXPECT_TRUE(is_line_image("image/png", "a"));
    EXPECT_TRUE(is_line_image("", "photo.JPG"));
    EXPECT_FALSE(is_line_image("image/gif", "a.gif"));
    EXPECT_FALSE(is_line_image("application/pdf", "a.png"));
    EXPECT_EQ(safe_file_name(u8"截图 1.png"), "_______1.png");
    EXPECT_EQ(safe_file_name("../../etc/passwd"), "_.._etc_passwd");
    EXPECT_EQ(safe_file_name("...."), "file");
    const auto long_name = safe_file_name(std::string(100, 'x') + ".jpeg");
    EXPECT_EQ(long_name.size(), 64u);
    EXPECT_EQ(long_name.substr(long_name.size() - 5), ".jpeg");
}

// 场景:登记一张要发送的图片,再按 URL 路径查找;另有过期、文件名不符、路径格式不对、令牌不存在的请求。
// 期望:只有令牌与文件名都对、且未过期才返回登记时的路径;URL 内容绝不参与路径拼接。
TEST(LineMedia, RegistryFindsOnlyExactLiveEntries) {
    using Clock = MediaRegistry::Clock;
    MediaRegistry registry(std::chrono::minutes(30), 2);
    const auto now = Clock::now();
    const auto relative = registry.add("C:/data/a.png", "a.png", "image/png", now);
    ASSERT_FALSE(relative.empty());
    const auto slash = relative.find('/');
    const auto token = relative.substr(0, slash);
    EXPECT_EQ(token.size(), 32u);
    EXPECT_EQ(relative.substr(slash + 1), "a.png");

    const auto found = registry.find_path(std::string(kMediaRoutePrefix) + relative, now);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->path, std::filesystem::path("C:/data/a.png"));
    EXPECT_EQ(found->mime_type, "image/png");

    EXPECT_FALSE(registry.find(token, "b.png", now).has_value());
    EXPECT_FALSE(registry.find_path("/line/media/" + token + "/../a.png", now).has_value());
    EXPECT_FALSE(registry.find_path("/line/media/" + token, now).has_value());
    EXPECT_FALSE(registry.find_path("/other/" + relative, now).has_value());
    EXPECT_FALSE(registry.find("nosuchtoken", "a.png", now).has_value());
    EXPECT_FALSE(registry.find(token, "a.png", now + std::chrono::minutes(31)).has_value());
    // 过期的条目在查找时被移除。
    EXPECT_EQ(registry.size(), 0u);

    // 超过条数上限时丢最旧的。
    registry.add("1.png", "1.png", "image/png", now);
    registry.add("2.png", "2.png", "image/png", now + std::chrono::seconds(1));
    registry.add("3.png", "3.png", "image/png", now + std::chrono::seconds(2));
    EXPECT_LE(registry.size(), 2u);
}

} // namespace
} // namespace acecode::im::line
