#include <gtest/gtest.h>

#include "im/feishu/feishu_protocol.hpp"

#include <chrono>

// im/feishu/feishu_protocol:飞书协议纯逻辑 —— 接入地址、长连接地址响应与握手分类、
// OpenAPI 错误分类、im.message.receive_v1 解析(@ 判定、文本清理、附件)、出站 Markdown 与分段。

namespace acecode::im::feishu {
namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

const BotIdentity kBot{"ou_bot", "TestBot"};

nlohmann::json bot_mention(const std::string& key = "@_user_1") {
    return {{"key", key}, {"id", {{"open_id", "ou_bot"}, {"union_id", "on_b"}}}, {"mentioned_type", "bot"},
            {"name", "TestBot"}, {"tenant_key", "tk"}};
}

nlohmann::json user_mention(const std::string& key, const std::string& open_id, const std::string& name) {
    return {{"key", key}, {"id", {{"open_id", open_id}}}, {"mentioned_type", "user"}, {"name", name}};
}

std::string event(const std::string& chat_type, const std::string& message_type, const nlohmann::json& content,
                  const nlohmann::json& mentions = nullptr, std::int64_t create_time = 0,
                  const std::string& sender_type = "user", const std::string& sender = "ou_user") {
    nlohmann::json message{{"message_id", "om_1"},
                           {"create_time", std::to_string(create_time ? create_time : now_ms())},
                           {"chat_id", "oc_chat"},
                           {"chat_type", chat_type},
                           {"message_type", message_type},
                           {"content", content.dump()}};
    if (!mentions.is_null()) message["mentions"] = mentions;
    return nlohmann::json{{"schema", "2.0"},
                          {"header", {{"event_id", "e1"}, {"event_type", "im.message.receive_v1"}}},
                          {"event", {{"sender", {{"sender_id", {{"open_id", sender}}}, {"sender_type", sender_type}}},
                                     {"message", message}}}}
        .dump();
}

Inbound parse_ok(const std::string& payload, const BotIdentity& bot = kBot) {
    auto parsed = parse_event(payload, bot, "cli_app", now_ms());
    EXPECT_EQ(parsed.disposition, EventDisposition::Message);
    return parsed.inbound.value_or(Inbound{});
}

// 场景:用户选择“飞书”或“Lark”(含大小写与空白差异)。
// 期望:lark → open.larksuite.com,其余(含空串)→ open.feishu.cn。
TEST(FeishuProtocol, BaseForDomain) {
    EXPECT_EQ(base_for_domain("lark"), kLarkBase);
    EXPECT_EQ(base_for_domain(" LARK "), kLarkBase);
    EXPECT_EQ(base_for_domain("feishu"), kFeishuBase);
    EXPECT_EQ(base_for_domain(""), kFeishuBase);
}

// 场景:长连接地址接口成功返回 URL 与 ClientConfig。
// 期望:取出 URL、device_id、service_id(int32),ClientConfig 原样保留以便应用。
TEST(FeishuProtocol, ParsesEndpointSuccess) {
    const auto body = nlohmann::json{
        {"code", 0},
        {"msg", "ok"},
        {"data",
         {{"URL", "wss://msg-frontier.feishu.cn/ws/v2?fpid=1&aid=2&device_id=7123&access_key=k&service_id="
                  "33554678&ticket=t%2B1"},
          {"ClientConfig", {{"ReconnectCount", -1}, {"ReconnectInterval", 120}, {"ReconnectNonce", 30},
                            {"PingInterval", 90}}}}}};
    const auto info = parse_endpoint_response(200, body.dump());
    ASSERT_TRUE(info.ok) << info.message;
    EXPECT_FALSE(info.fatal);
    EXPECT_EQ(info.device_id, "7123");
    EXPECT_EQ(info.service_id, 33554678);
    EXPECT_EQ(url_query_param(info.url, "ticket"), "t+1");
    ClientConfig config;
    EXPECT_TRUE(apply_client_config(info.client_config, &config));
    EXPECT_EQ(config.ping_interval_s, 90);
}

// 场景:长连接地址接口的各种失败 —— 原样抓包的凭据错误(HTTP 200 + code 1000040345)、
// 服务繁忙(1、1000040343)、HTTP 500、code 0 但 URL 为空。
// 期望:凭据错误是致命的,给出中文原因;其余都可重试。
TEST(FeishuProtocol, ClassifiesEndpointFailures) {
    const auto probe =
        R"({"error":{"id":1000040345,"localizedMessage":{"locale":"zh","message":"app_id or app_secret is invalid"},"ErrX":null},"msg":"app_id or app_secret is invalid","data":{"URL":""},"code":1000040345})";
    const auto bad = parse_endpoint_response(200, probe);
    EXPECT_FALSE(bad.ok);
    EXPECT_TRUE(bad.fatal);
    EXPECT_NE(bad.message.find("App Secret"), std::string::npos) << bad.message;

    for (const auto code : {1LL, 1000040343LL}) {
        const auto busy = parse_endpoint_response(200, nlohmann::json{{"code", code}, {"msg", "busy"}}.dump());
        EXPECT_FALSE(busy.ok);
        EXPECT_FALSE(busy.fatal) << code;
    }
    EXPECT_FALSE(parse_endpoint_response(500, "oops").fatal);
    const auto empty = parse_endpoint_response(200, R"({"code":0,"data":{"URL":""}})");
    EXPECT_FALSE(empty.ok);
    EXPECT_FALSE(empty.fatal);
    const auto other = parse_endpoint_response(200, R"({"code":1000040344,"msg":"no credential"})");
    EXPECT_TRUE(other.fatal);
}

// 场景:服务端只下发部分 ClientConfig 字段,或给出非法值(负的间隔、0 的心跳)。
// 期望:只覆盖出现且合法的字段,其余保持默认。
TEST(FeishuProtocol, ClientConfigOverridesOnlyValidFields) {
    ClientConfig config;
    EXPECT_TRUE(apply_client_config({{"PingInterval", 30}}, &config));
    EXPECT_EQ(config.ping_interval_s, 30);
    EXPECT_EQ(config.reconnect_interval_s, 120);
    EXPECT_FALSE(apply_client_config({{"ReconnectInterval", -5}, {"PingInterval", 0}}, &config));
    EXPECT_EQ(config.reconnect_interval_s, 120);
    EXPECT_EQ(config.ping_interval_s, 30);
    EXPECT_TRUE(apply_client_config({{"ReconnectCount", 3}, {"ReconnectNonce", 0}}, &config));
    EXPECT_EQ(config.reconnect_count, 3);
    EXPECT_EQ(config.reconnect_nonce_s, 0);
}

// 场景:WebSocket 升级失败时的 Handshake-Status / Handshake-Autherrcode 组合。
// 期望:403 与 514+1000040350(连接数超限)致命;514 的其它错误码与拿不到头时都可重试。
TEST(FeishuProtocol, ClassifiesHandshakeFailures) {
    EXPECT_TRUE(classify_handshake(403, 403, 0).fatal);
    EXPECT_TRUE(classify_handshake(400, 514, 1000040350).fatal);
    EXPECT_FALSE(classify_handshake(400, 514, 123).fatal);
    EXPECT_FALSE(classify_handshake(502, 0, 0).fatal);
    EXPECT_FALSE(classify_handshake(0, 0, 0).message.empty());
}

// 场景:私聊里收到一条文本消息。
// 期望:地址为私聊,chat = sender = 对方 open_id,account = App ID;私聊恒视为点名;
// reply_context 带 message_id / chat_id / chat_type。
TEST(FeishuProtocol, ParsesPrivateTextMessage) {
    const auto inbound = parse_ok(event("p2p", "text", {{"text", "  你好\r\n世界  "}}));
    EXPECT_EQ(inbound.address.platform, "feishu");
    EXPECT_EQ(inbound.address.account, "cli_app");
    EXPECT_EQ(inbound.address.kind, ChatKind::Private);
    EXPECT_EQ(inbound.address.chat, "ou_user");
    EXPECT_EQ(inbound.address.sender, "ou_user");
    EXPECT_TRUE(inbound.mentioned);
    EXPECT_EQ(inbound.text, "你好\n世界");
    EXPECT_EQ(inbound.message_id, "om_1");
    EXPECT_EQ(inbound.reply_context.value("message_id", ""), "om_1");
    EXPECT_EQ(inbound.reply_context.value("chat_id", ""), "oc_chat");
    EXPECT_EQ(inbound.reply_context.value("chat_type", ""), "p2p");
}

// 场景:群里 "@机器人 帮我看看"。
// 期望:按 open_id 认出点名了机器人,mentioned = true;开头的 @ 去掉;地址为群 + 发言人。
TEST(FeishuProtocol, GroupMentionOfBotIsDetectedAndStripped) {
    const auto inbound =
        parse_ok(event("group", "text", {{"text", "@_user_1 帮我看看"}}, nlohmann::json::array({bot_mention()})));
    EXPECT_EQ(inbound.address.kind, ChatKind::Group);
    EXPECT_EQ(inbound.address.chat, "oc_chat");
    EXPECT_EQ(inbound.address.sender, "ou_user");
    EXPECT_TRUE(inbound.mentioned);
    EXPECT_EQ(inbound.text, "帮我看看");
}

// 场景:群消息只 @ 了别人,且机器人名字恰好与被 @ 的人相同(open_id 不同)。
// 期望:open_id 不同就不算点名机器人(名字只是最后手段);被 @ 的人显示为 "@名字"。
TEST(FeishuProtocol, MentionOfAnotherUserIsNotTheBot) {
    const auto mentions = nlohmann::json::array({user_mention("@_user_1", "ou_tom", "TestBot")});
    const auto inbound = parse_ok(event("group", "text", {{"text", "@_user_1 你好"}}, mentions));
    EXPECT_FALSE(inbound.mentioned);
    EXPECT_EQ(inbound.text, "@TestBot 你好");
}

// 场景:机器人的 @ 出现在句中、句尾(后跟全角问号)、以及 "@机器人, 帮忙" 这种带分隔符的开头。
// 期望:句中保留为 "@机器人名",句尾去掉但保留标点,开头去掉并吃掉紧跟的逗号。
TEST(FeishuProtocol, StripsBotMentionsOnlyAtEdges) {
    const auto mentions = std::vector<Mention>{{"@_user_1", "ou_bot", "", "TestBot", "bot"}};
    EXPECT_EQ(clean_text("别再 @_user_1 了", mentions, kBot), "别再 @TestBot 了");
    EXPECT_EQ(clean_text("看一下 @_user_1\xEF\xBC\x9F", mentions, kBot), "看一下\xEF\xBC\x9F");
    EXPECT_EQ(clean_text("@_user_1, 帮忙", mentions, kBot), "帮忙");
    EXPECT_EQ(clean_text("@_user_1 @_user_1 两次", mentions, kBot), "两次");
    // @_user_10 不能被当成 @_user_1 的前缀吃掉。
    EXPECT_EQ(clean_text("@_user_10 hi", mentions, kBot), "hi");
}

// 场景:@所有人,以及 mentions 里查不到的占位符。
// 期望:@所有人不算点名机器人(官方渠道 SDK 默认),文字里显示为 "@all";未知占位符换成空格。
TEST(FeishuProtocol, AtAllAndUnknownPlaceholders) {
    const auto inbound = parse_ok(
        event("group", "text", {{"text", "@_all 开会"}}, nlohmann::json::array({{{"key", "@_all"}, {"name", "所有人"}}})));
    EXPECT_FALSE(inbound.mentioned);
    EXPECT_EQ(inbound.text, "@all 开会");
    EXPECT_EQ(clean_text("a@_user_9b", {}, kBot), "a b");
    EXPECT_EQ(clean_text("@_allx", {}, kBot), "@_allx");
}

// 场景:还没取到机器人身份(未发布版本时 bot/v3/info 不可用),群里 @ 了一个 mentioned_type=bot 的对象。
// 期望:退化为按 mentioned_type 判定,仍算点名并去掉 @。
TEST(FeishuProtocol, UnknownBotIdentityFallsBackToMentionType) {
    const auto inbound = parse_ok(
        event("group", "text", {{"text", "@_user_1 在吗"}}, nlohmann::json::array({bot_mention()})), BotIdentity{});
    EXPECT_TRUE(inbound.mentioned);
    EXPECT_EQ(inbound.text, "在吗");
}

// 场景:平台在重启后重投 31 分钟前的消息;另一条是 29 分钟前的。
// 期望:超过 30 分钟的判为陈旧并丢弃,29 分钟的照常处理。
TEST(FeishuProtocol, DropsStaleEvents) {
    const auto stale = parse_event(event("p2p", "text", {{"text", "hi"}}, nullptr, now_ms() - 31 * 60 * 1000), kBot,
                                   "cli_app", now_ms());
    EXPECT_EQ(stale.disposition, EventDisposition::Stale);
    EXPECT_EQ(stale.message_id, "om_1");
    const auto fresh = parse_event(event("p2p", "text", {{"text", "hi"}}, nullptr, now_ms() - 29 * 60 * 1000), kBot,
                                   "cli_app", now_ms());
    EXPECT_EQ(fresh.disposition, EventDisposition::Message);
}

// 场景:机器人 / 应用发出的消息,以及机器人自己的回声;只有 "@机器人" 没有内容的消息。
// 期望:前两者判为 FromBot,最后一个判为 Empty,都不交给核心。
TEST(FeishuProtocol, IgnoresBotSendersAndBareMentions) {
    EXPECT_EQ(parse_event(event("group", "text", {{"text", "hi"}}, nullptr, 0, "app"), kBot, "cli_app", now_ms())
                  .disposition,
              EventDisposition::FromBot);
    EXPECT_EQ(parse_event(event("p2p", "text", {{"text", "hi"}}, nullptr, 0, "user", "ou_bot"), kBot, "cli_app",
                          now_ms())
                  .disposition,
              EventDisposition::FromBot);
    EXPECT_EQ(parse_event(event("group", "text", {{"text", "@_user_1"}}, nlohmann::json::array({bot_mention()})), kBot,
                          "cli_app", now_ms())
                  .disposition,
              EventDisposition::Empty);
}

// 场景:其它事件类型、1.0 事件、损坏的 JSON。
// 期望:前两者 Ignored(仍由传输层 ACK),损坏的 Invalid。
TEST(FeishuProtocol, IgnoresOtherEvents) {
    const auto reaction =
        nlohmann::json{{"schema", "2.0"}, {"header", {{"event_type", "im.message.reaction.created_v1"}}}, {"event", {}}};
    EXPECT_EQ(parse_event(reaction.dump(), kBot, "cli_app", now_ms()).disposition, EventDisposition::Ignored);
    EXPECT_EQ(parse_event(R"({"type":"event_callback","uuid":"u","event":{}})", kBot, "cli_app", now_ms()).disposition,
              EventDisposition::Ignored);
    EXPECT_EQ(parse_event("{not json", kBot, "cli_app", now_ms()).disposition, EventDisposition::Invalid);
}

// 场景:post 富文本,含标题、普通文字、链接、@机器人、图片、代码块;同时测试外层包了 zh_cn 的形态。
// 期望:文字按行拼接(@机器人 在开头被去掉),图片成为附件且下载定位指向这条消息、type=image;
// 代码块还原成 ``` 围栏;两种形态结果一致。
TEST(FeishuProtocol, ExtractsPostTextAndImages) {
    const nlohmann::json post{
        {"title", "周报"},
        {"content",
         nlohmann::json::array(
             {nlohmann::json::array({{{"tag", "at"}, {"user_id", "@_user_1"}, {"user_name", ""}},
                                     {{"tag", "text"}, {"text", " 看下"}},
                                     {{"tag", "a"}, {"text", "文档"}, {"href", "https://a.b/c"}}}),
              nlohmann::json::array({{{"tag", "img"}, {"image_key", "img_v3_1"}}}),
              nlohmann::json::array({{{"tag", "code_block"}, {"language", "GO"}, {"text", "x := 1"}}})})}};
    const auto mentions = nlohmann::json::array({bot_mention()});
    for (const auto& content : {post, nlohmann::json{{"zh_cn", post}}}) {
        const auto inbound = parse_ok(event("group", "post", content, mentions));
        EXPECT_TRUE(inbound.mentioned);
        EXPECT_EQ(inbound.text, "周报\n@TestBot 看下[文档](https://a.b/c)\n```go\nx := 1\n```");
        ASSERT_EQ(inbound.attachments.size(), 1u);
        EXPECT_EQ(inbound.attachments[0].kind, AttachmentKind::Image);
        const auto ref = parse_resource_ref(inbound.attachments[0].remote_ref);
        ASSERT_TRUE(ref.has_value());
        EXPECT_EQ(ref->message_id, "om_1");
        EXPECT_EQ(ref->key, "img_v3_1");
        EXPECT_EQ(ref->type, "image");
    }
}

// 场景:图片、文件、语音、视频、表情包、文件夹消息。
// 期望:图片 / 文件 / 语音 / 视频成为对应种类的附件(文件保留原名,下载 type 分别为 image / file);
// 表情包与文件夹无法下载,转成占位文字。
TEST(FeishuProtocol, MediaMessagesBecomeAttachments) {
    const auto image = extract_content("image", {{"image_key", "img_k"}}, "om_9");
    ASSERT_EQ(image.attachments.size(), 1u);
    EXPECT_EQ(image.attachments[0].kind, AttachmentKind::Image);
    EXPECT_EQ(parse_resource_ref(image.attachments[0].remote_ref)->type, "image");

    const auto file = extract_content("file", {{"file_key", "file_k"}, {"file_name", "报告.pdf"}}, "om_9");
    ASSERT_EQ(file.attachments.size(), 1u);
    EXPECT_EQ(file.attachments[0].kind, AttachmentKind::File);
    EXPECT_EQ(file.attachments[0].name, "报告.pdf");
    EXPECT_EQ(parse_resource_ref(file.attachments[0].remote_ref)->type, "file");

    EXPECT_EQ(extract_content("audio", {{"file_key", "a"}, {"duration", 2000}}, "om_9").attachments.at(0).kind,
              AttachmentKind::Voice);
    EXPECT_EQ(extract_content("media", {{"file_key", "v"}, {"image_key", "c"}}, "om_9").attachments.at(0).kind,
              AttachmentKind::Video);
    const auto sticker = extract_content("sticker", {{"file_key", "s"}}, "om_9");
    EXPECT_TRUE(sticker.attachments.empty());
    EXPECT_FALSE(sticker.text.empty());
    EXPECT_NE(extract_content("folder", {{"file_key", "f"}, {"file_name", "资料"}}, "om_9").text.find("资料"),
              std::string::npos);
}

// 场景:卡片消息(interactive)的简化内容。
// 期望:收集 title / text / content 字段的文字,按行拼接,去掉相邻重复。
TEST(FeishuProtocol, FlattensInteractiveCards) {
    const nlohmann::json card{
        {"title", "审批"},
        {"elements", nlohmann::json::array({nlohmann::json::array(
                         {{{"tag", "text"}, {"text", "同意吗"}}, {{"tag", "text"}, {"text", "同意吗"}}})})}};
    EXPECT_EQ(extract_content("interactive", card, "om_1").text, "审批\n同意吗");
}

// 场景:附件定位串被篡改或缺字段。
// 期望:解析失败返回空,下载时给出中文原因而不是发出错误请求。
TEST(FeishuProtocol, ResourceRefRejectsInvalidValues) {
    EXPECT_TRUE(parse_resource_ref(make_resource_ref("om_1", "k", "file")).has_value());
    EXPECT_FALSE(parse_resource_ref(make_resource_ref("om_1", "k", "video")).has_value());
    EXPECT_FALSE(parse_resource_ref(make_resource_ref("", "k", "file")).has_value());
    EXPECT_FALSE(parse_resource_ref("https://example.com/x").has_value());
}

// 场景:各种助手回复,判定是否按 Markdown 发送。
// 期望:表格、标题、列表、分隔线、代码、粗体、删除线、下划线、斜体、链接、引用都判为 Markdown;
// 普通句子、网址、"1.5 倍"、单个星号不判。
TEST(FeishuProtocol, DetectsMarkdown) {
    for (const char* text : {"| a | b |\n|---|---|\n| 1 | 2 |", "# 标题", "- 列表", "  * 列表", "1. 第一",
                             "---", "```\ncode\n```", "用 `ls` 看", "这是 **重点** 内容", "~~删掉~~", "<u>下划线</u>",
                             "*斜体*", "见 [文档](https://a.b)", "> 引用"}) {
        EXPECT_TRUE(looks_like_markdown(text)) << text;
    }
    for (const char* text : {"你好,今天天气不错。", "https://example.com/a_b", "放大 1.5 倍", "3 * 4 = 12", "**",
                             "#没有空格"}) {
        EXPECT_FALSE(looks_like_markdown(text)) << text;
    }
}

// 场景:带代码块的 Markdown 回复转成 post。
// 期望:{"zh_cn":{"content":rows}}(没有外层 "post" 包装,否则 230001);代码块单独占一行 md 节点,
// 前后正文各占一行;没有代码块时整段一个 md 节点。
TEST(FeishuProtocol, BuildsPostWithIsolatedCodeBlocks) {
    const auto post = build_post_content("说明\n```cpp\nint a;\n```\n结论");
    ASSERT_TRUE(post.contains("zh_cn"));
    EXPECT_FALSE(post.contains("post"));
    const auto& rows = post["zh_cn"]["content"];
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0][0]["tag"], "md");
    EXPECT_EQ(rows[0][0]["text"], "说明");
    EXPECT_EQ(rows[1][0]["text"], "```cpp\nint a;\n```");
    EXPECT_EQ(rows[2][0]["text"], "结论");
    const auto simple = build_post_content("**粗体**");
    ASSERT_EQ(simple["zh_cn"]["content"].size(), 1u);
    EXPECT_EQ(simple["zh_cn"]["content"][0][0]["text"], "**粗体**");
    EXPECT_EQ(build_text_content("a\nb")["text"], "a\nb");
}

// 场景:超长回复与转义膨胀严重的回复(全是双引号:每个字符序列化后占 2 字节)。
// 期望:每段不超过字符上限;每段 content 序列化后不超过字节上限;拼回去与原文一致。
TEST(FeishuProtocol, SplitsByCharactersAndSerializedBytes) {
    const std::string long_text(8000, 'x');
    const auto chunks = split_outbound(long_text, false);
    ASSERT_GE(chunks.size(), 3u);
    for (const auto& chunk : chunks) EXPECT_LE(chunk.size(), kMaxTextChars);

    const std::string quotes(3000, '"');
    const auto pieces = split_outbound(quotes, false, 3500, 4096);
    ASSERT_GE(pieces.size(), 2u);
    std::string joined;
    for (const auto& piece : pieces) {
        EXPECT_LE(dump_json(build_text_content(piece)).size(), 4096u);
        joined += piece;
    }
    EXPECT_EQ(joined, quotes);
}

// 场景:OpenAPI 的各种响应。
// 期望:只有 2xx 且 code 0 才算成功;429 / 99991400 / 230020 为限流;网络错误、5xx 为临时错误;
// 230001 或 "content format of the post type is incorrect" 为 post 被拒;令牌失效码与回复目标消失码可识别;
// 错误描述为中文且不暴露内部字段。
TEST(FeishuProtocol, ClassifiesApiResults) {
    EXPECT_TRUE(parse_api_response(200, R"({"code":0,"msg":"success","data":{"message_id":"om"}})").ok);
    EXPECT_EQ(parse_api_response(200, R"({"code":0,"data":{"message_id":"om"}})").data.value("message_id", ""), "om");
    EXPECT_FALSE(parse_api_response(200, R"({"code":230013,"msg":"x"})").ok);
    EXPECT_FALSE(parse_api_response(200, "not json").ok);

    EXPECT_TRUE(is_rate_limited(parse_api_response(429, R"({"code":99991400})")));
    EXPECT_TRUE(is_rate_limited(parse_api_response(400, R"({"code":230020})")));
    EXPECT_TRUE(is_transient(parse_api_response(502, "")));
    EXPECT_TRUE(is_transient(ApiResult{}));
    EXPECT_FALSE(is_transient(parse_api_response(400, R"({"code":230013})")));
    EXPECT_TRUE(is_post_rejected(parse_api_response(400, R"({"code":230001,"msg":"invalid param"})")));
    EXPECT_TRUE(is_post_rejected(
        parse_api_response(400, R"({"code":9,"msg":"The content format of the post type is incorrect"})")));
    EXPECT_TRUE(is_token_invalid(99991663));
    EXPECT_TRUE(is_token_invalid(99991661));
    EXPECT_FALSE(is_token_invalid(230013));
    EXPECT_TRUE(is_reply_target_gone(230011));
    EXPECT_TRUE(is_reply_target_gone(231003));

    EXPECT_NE(describe_error(parse_api_response(400, R"({"code":230013,"msg":"x"})")).find("可用范围"),
              std::string::npos);
    EXPECT_NE(describe_error(parse_api_response(400, R"({"code":99991672,"msg":"need im:message"})")).find(
                  "im:message"),
              std::string::npos);
    EXPECT_NE(describe_error(parse_api_response(400, R"({"code":777,"msg":"boom"})")).find("777"), std::string::npos);
    ApiResult network;
    network.error = "Couldn't connect";
    EXPECT_NE(describe_error(network).find("无法连接"), std::string::npos);
}

// 场景:限流响应头 x-ogw-ratelimit-reset 的各种取值,以及据此计算重发前的等待。
// 期望:合法秒数原样取出(上限 1 小时),非法 / 缺失为 0;等待取本地退避与平台要求的较大者,
// 平台要求封顶 60 秒(避免一条消息卡住整个会话太久)。
TEST(FeishuProtocol, ComputesRateLimitWait) {
    using std::chrono::milliseconds;
    using std::chrono::seconds;
    EXPECT_EQ(parse_retry_after(" 3 "), seconds(3));
    EXPECT_EQ(parse_retry_after(""), seconds(0));
    EXPECT_EQ(parse_retry_after("1.5"), seconds(0));
    EXPECT_EQ(parse_retry_after("99999"), seconds(3600));
    ApiResult limited;
    limited.retry_after = seconds(5);
    EXPECT_EQ(rate_limit_wait(limited, milliseconds(1000)), milliseconds(5000));
    limited.retry_after = seconds(120);
    EXPECT_EQ(rate_limit_wait(limited, milliseconds(1000)), milliseconds(60000));
    limited.retry_after = seconds(0);
    EXPECT_EQ(rate_limit_wait(limited, milliseconds(1000)), milliseconds(1000));
}

// 场景:换令牌接口返回的凭据错误码。
// 期望:10014 提示确认飞书 / Lark;10015 提示 App Secret 不正确;其它带上平台原文与错误码。
TEST(FeishuProtocol, DescribesTokenErrors) {
    EXPECT_NE(describe_token_error(10014, "app id not exists").find("Lark"), std::string::npos);
    EXPECT_NE(describe_token_error(10015, "wrong app secret").find("App Secret"), std::string::npos);
    EXPECT_NE(describe_token_error(12345, "weird").find("12345"), std::string::npos);
}

// 场景:bot/v3/info 的三种形态 —— bot 在顶层、在 data 下、机器人未启用。
// 期望:前两种都能取出 open_id 与名称;activate_status != 2 时 ready() 为 false 并有中文提示。
TEST(FeishuProtocol, ParsesBotInfo) {
    const auto top = parse_bot_info(
        nlohmann::json::parse(R"({"code":0,"bot":{"activate_status":2,"app_name":"A","open_id":"ou_b"}})"));
    EXPECT_TRUE(top.ready());
    EXPECT_EQ(top.open_id, "ou_b");
    EXPECT_EQ(top.name, "A");
    const auto nested =
        parse_bot_info(nlohmann::json::parse(R"({"code":0,"data":{"bot":{"activate_status":2,"open_id":"ou_c"}}})"));
    EXPECT_EQ(nested.open_id, "ou_c");
    const auto inactive =
        parse_bot_info(nlohmann::json::parse(R"({"code":0,"bot":{"activate_status":0,"open_id":"ou_d"}})"));
    EXPECT_TRUE(inactive.ok);
    EXPECT_FALSE(inactive.ready());
    EXPECT_FALSE(describe_activate_status(0).empty());
    EXPECT_TRUE(describe_activate_status(2).empty());
    EXPECT_FALSE(parse_bot_info(nlohmann::json::parse(R"({"code":99991672,"msg":"x"})")).ok);
}

// 场景:有回复上下文(群 / 私聊 / 话题)与没有回复上下文(Desktop 里输入的回复)时的出站目标。
// 期望:有 chat_id 一律按 chat_id 发(私聊也有);没有时群按 chat_id = 群 id、私聊按 open_id = 对方 id;
// 话题消息标记 in_thread;没有 message_id 时不回复。
TEST(FeishuProtocol, SelectsSendTarget) {
    Address group{"feishu", "cli_app", ChatKind::Group, "oc_g", "ou_u", ""};
    Address dm{"feishu", "cli_app", ChatKind::Private, "ou_u", "ou_u", ""};
    const auto with_context = send_target(dm, {{"message_id", "om_1"}, {"chat_id", "oc_p2p"}, {"chat_type", "p2p"}});
    EXPECT_EQ(with_context.receive_id_type, "chat_id");
    EXPECT_EQ(with_context.receive_id, "oc_p2p");
    EXPECT_EQ(with_context.reply_to, "om_1");
    EXPECT_FALSE(with_context.in_thread);
    const auto plain_group = send_target(group, nlohmann::json::object());
    EXPECT_EQ(plain_group.receive_id_type, "chat_id");
    EXPECT_EQ(plain_group.receive_id, "oc_g");
    EXPECT_TRUE(plain_group.reply_to.empty());
    const auto plain_dm = send_target(dm, nullptr);
    EXPECT_EQ(plain_dm.receive_id_type, "open_id");
    EXPECT_EQ(plain_dm.receive_id, "ou_u");
    EXPECT_TRUE(send_target(group, {{"message_id", "om_2"}, {"chat_id", "oc_g"}, {"thread_id", "omt_1"}}).in_thread);
}

// 场景:回传各种文件。
// 期望:≤ 10 MB 的图片走图片接口;超过 10 MB 的图片按普通文件(stream);.opus → 语音,.mp4 → 视频,
// pdf / Office 各自类型,其余 stream;上传类型与 msg_type 配套。
TEST(FeishuProtocol, RoutesFilesForUpload) {
    EXPECT_TRUE(route_file("a.png", "", 1000).image);
    EXPECT_TRUE(route_file("photo", "image/jpeg", 1000).image);
    const auto big = route_file("a.png", "image/png", 11u * 1024u * 1024u);
    EXPECT_FALSE(big.image);
    EXPECT_EQ(big.file_type, "stream");
    EXPECT_EQ(big.msg_type, "file");
    EXPECT_EQ(route_file("v.opus", "", 10).msg_type, "audio");
    EXPECT_EQ(route_file("v.opus", "", 10).file_type, "opus");
    EXPECT_EQ(route_file("m.MP4", "", 10).msg_type, "media");
    EXPECT_EQ(route_file("r.pdf", "", 10).file_type, "pdf");
    EXPECT_EQ(route_file("r.docx", "", 10).file_type, "doc");
    EXPECT_EQ(route_file("r.xlsx", "", 10).file_type, "xls");
    EXPECT_EQ(route_file("r.pptx", "", 10).file_type, "ppt");
    EXPECT_EQ(route_file("r.zip", "", 10).file_type, "stream");
    EXPECT_FALSE(route_file("vector.svg", "image/svg+xml", 10).image);
}

} // namespace
} // namespace acecode::im::feishu
