#include <gtest/gtest.h>

#include "im/dingtalk/dingtalk_protocol.hpp"

// im/dingtalk/dingtalk_protocol:钉钉机器人消息解析、错误分类、webhook 白名单、
// Markdown 标题与渲染怪癖修正。全部是纯函数,不碰网络。

namespace acecode::im::dingtalk {
namespace {

constexpr const char* kClient = "dingCLIENT";

nlohmann::json private_text(const std::string& text) {
    return {{"msgId", "msgA"},
            {"msgtype", "text"},
            {"text", {{"content", text}}},
            {"conversationType", "1"},
            {"conversationId", "cidP+/x=="},
            {"senderId", "$:LWCP_v1:$abc=="},
            {"senderStaffId", "staff01"},
            {"senderNick", "Ann"},
            {"robotCode", kClient},
            {"sessionWebhook", "https://oapi.dingtalk.com/robot/sendBySession?session=s1"},
            {"sessionWebhookExpiredTime", 1700000000000LL}};
}

nlohmann::json group_text(const std::string& text) {
    return {{"msgId", "msgG"},
            {"msgtype", "text"},
            {"text", {{"content", text}}},
            {"conversationType", "2"},
            {"conversationId", "cidGROUP+/=="},
            {"senderId", "$:LWCP_v1:$bob"},
            {"senderStaffId", "staff02"},
            {"chatbotUserId", "$:LWCP_v1:$bot"},
            {"isInAtList", true},
            {"atUsers", nlohmann::json::array({{{"dingtalkId", "$:LWCP_v1:$bot"}}})}};
}

// 场景:组织内用户单聊机器人发一条文字。
// 期望:私聊地址 chat = sender = senderStaffId,account = Client ID;恒视为点名;
// 文本去掉首尾空白;回复上下文带 webhook、到期时间、会话类型与 msgId。
TEST(DingTalkProtocol, ParsesPrivateTextWithStaffId) {
    const auto inbound = parse_robot_message(private_text("  你好  "), kClient);
    ASSERT_TRUE(inbound);
    EXPECT_EQ(inbound->address.platform, "dingtalk");
    EXPECT_EQ(inbound->address.account, kClient);
    EXPECT_EQ(inbound->address.kind, ChatKind::Private);
    EXPECT_EQ(inbound->address.chat, "staff01");
    EXPECT_EQ(inbound->address.sender, "staff01");
    EXPECT_TRUE(inbound->mentioned);
    EXPECT_EQ(inbound->text, u8"你好");
    EXPECT_EQ(inbound->message_id, "msgA");
    EXPECT_EQ(inbound->sender_name, "Ann");
    const auto& ctx = inbound->reply_context;
    EXPECT_EQ(ctx.value("sessionWebhook", ""), "https://oapi.dingtalk.com/robot/sendBySession?session=s1");
    EXPECT_EQ(ctx.value("sessionWebhookExpiredTime", 0LL), 1700000000000LL);
    EXPECT_EQ(ctx.value("conversationType", ""), "1");
    EXPECT_EQ(ctx.value("senderStaffId", ""), "staff01");
    EXPECT_EQ(ctx.value("msgId", ""), "msgA");
    EXPECT_EQ(ctx.value("robotCode", ""), kClient);
}

// 场景:组织外用户单聊(没有 senderStaffId),conversationType 以数字 1 发来。
// 期望:回退用加密 senderId 作为 chat 与 sender;数字形式的会话类型也能识别为单聊。
TEST(DingTalkProtocol, ExternalUserFallsBackToSenderId) {
    auto data = private_text("hi");
    data.erase("senderStaffId");
    data["conversationType"] = 1;
    const auto inbound = parse_robot_message(data, kClient);
    ASSERT_TRUE(inbound);
    EXPECT_EQ(inbound->address.chat, "$:LWCP_v1:$abc==");
    EXPECT_TRUE(is_encrypted_sender_id(inbound->address.chat));
    EXPECT_EQ(inbound->reply_context.value("senderStaffId", "x"), "");
}

// 场景:群里 @机器人 并在开头带着机器人的 @名字(atUsers 只有机器人自己)。
// 期望:群地址 chat = conversationId、sender = staffId;mentioned=true;开头的 @名字被去掉。
TEST(DingTalkProtocol, ParsesGroupMentionAndStripsBotName) {
    const auto inbound = parse_robot_message(group_text("@ACE助手 /new 开始"), kClient);
    ASSERT_TRUE(inbound);
    EXPECT_EQ(inbound->address.kind, ChatKind::Group);
    EXPECT_EQ(inbound->address.chat, "cidGROUP+/==");
    EXPECT_EQ(inbound->address.sender, "staff02");
    EXPECT_TRUE(inbound->mentioned);
    EXPECT_EQ(inbound->text, "/new 开始");
}

// 场景:群消息里出现邮箱、git@host,或同时 @ 了别人。
// 期望:邮箱与 git@host 原样保留(@ 前不是空白);atUsers 里有别人时不去掉任何 @(分不清哪个是机器人)。
TEST(DingTalkProtocol, DoesNotStripEmailsOrAmbiguousMentions) {
    auto email = parse_robot_message(group_text("发到 a@b.com 和 git@github.com"), kClient);
    ASSERT_TRUE(email);
    EXPECT_EQ(email->text, "发到 a@b.com 和 git@github.com");

    auto data = group_text("@机器人 @张三 看看");
    data["atUsers"].push_back({{"dingtalkId", "$:LWCP_v1:$zhang"}});
    const auto both = parse_robot_message(data, kClient);
    ASSERT_TRUE(both);
    EXPECT_EQ(both->text, "@机器人 @张三 看看");
}

// 场景:@机器人 放在末尾;以及正文中间出现一个手打的 @文字(不是真正的提及)。
// 期望:末尾的 @名字被去掉;中间的 @文字原样保留(只去掉开头或末尾的提及,避免误删正文)。
TEST(DingTalkProtocol, StripsTrailingMentionButKeepsMiddleText) {
    const auto trailing = parse_robot_message(group_text("帮我看看 @ACE助手"), kClient);
    ASSERT_TRUE(trailing);
    EXPECT_EQ(trailing->text, "帮我看看");
    const auto middle = parse_robot_message(group_text("请转告 @老王 明天开会"), kClient);
    ASSERT_TRUE(middle);
    EXPECT_EQ(middle->text, "请转告 @老王 明天开会");
}

// 场景:群消息没有点名机器人(isInAtList=false,atUsers 不含机器人);以及两个字段都缺失。
// 期望:前者 mentioned=false;后者视为点名(企业内部机器人在群里只收得到 @ 它的消息)。
TEST(DingTalkProtocol, GroupMentionDetection) {
    auto data = group_text("随便聊聊");
    data["isInAtList"] = false;
    data["atUsers"] = nlohmann::json::array();
    const auto silent = parse_robot_message(data, kClient);
    ASSERT_TRUE(silent);
    EXPECT_FALSE(silent->mentioned);

    data.erase("isInAtList");
    data.erase("atUsers");
    const auto missing = parse_robot_message(data, kClient);
    ASSERT_TRUE(missing);
    EXPECT_TRUE(missing->mentioned);
}

// 场景:富文本消息,文字与图片交错。
// 期望:文字段按顺序用换行拼接;图片变成附件,remote_ref 为 downloadCode。
TEST(DingTalkProtocol, RichTextJoinsTextAndCollectsPictures) {
    auto data = private_text("");
    data.erase("text");
    data["msgtype"] = "richText";
    data["content"] = {{"richText", nlohmann::json::array({{{"text", "示例图如下："}},
                                                           {{"type", "picture"}, {"downloadCode", "DC1"},
                                                            {"pictureDownloadCode", "P1"}},
                                                           {{"text", "看完再说"}}})}};
    const auto inbound = parse_robot_message(data, kClient);
    ASSERT_TRUE(inbound);
    EXPECT_EQ(inbound->text, "示例图如下：\n看完再说");
    ASSERT_EQ(inbound->attachments.size(), 1u);
    EXPECT_EQ(inbound->attachments[0].kind, AttachmentKind::Image);
    EXPECT_EQ(inbound->attachments[0].remote_ref, "DC1");
}

// 场景:图片、文件、视频消息,其中文件消息的 content 以 JSON 字符串形式发来。
// 期望:分别解析为图片 / 文件(带文件名)/ 视频附件;字符串形式的 content 也能读。
TEST(DingTalkProtocol, MediaMessagesBecomeAttachments) {
    auto picture = private_text("");
    picture.erase("text");
    picture["msgtype"] = "picture";
    picture["content"] = {{"pictureDownloadCode", "P2"}, {"downloadCode", "DC2"}};
    const auto image = parse_robot_message(picture, kClient);
    ASSERT_TRUE(image);
    ASSERT_EQ(image->attachments.size(), 1u);
    EXPECT_EQ(image->attachments[0].kind, AttachmentKind::Image);
    EXPECT_EQ(image->attachments[0].remote_ref, "DC2");
    EXPECT_TRUE(image->text.empty());

    auto file = private_text("");
    file.erase("text");
    file["msgtype"] = "file";
    file["content"] = nlohmann::json{{"fileName", "报告.pdf"}, {"downloadCode", "DC3"}, {"spaceId", "1"}}.dump();
    const auto doc = parse_robot_message(file, kClient);
    ASSERT_TRUE(doc);
    ASSERT_EQ(doc->attachments.size(), 1u);
    EXPECT_EQ(doc->attachments[0].kind, AttachmentKind::File);
    EXPECT_EQ(doc->attachments[0].name, "报告.pdf");

    auto video = private_text("");
    video.erase("text");
    video["msgtype"] = "video";
    video["content"] = {{"duration", "5"}, {"videoType", "mp4"}, {"downloadCode", "DC4"}};
    const auto clip = parse_robot_message(video, kClient);
    ASSERT_TRUE(clip);
    EXPECT_EQ(clip->attachments.at(0).kind, AttachmentKind::Video);
    EXPECT_EQ(clip->attachments.at(0).name, "video.mp4");
}

// 场景:语音消息带平台识别文字。
// 期望:语音附件的 transcript 为识别文字(交给核心拼成“[语音转写]”),不当作需要重新转写的音频。
TEST(DingTalkProtocol, AudioCarriesRecognitionText) {
    auto audio = private_text("");
    audio.erase("text");
    audio["msgtype"] = "audio";
    audio["content"] = {{"downloadCode", "DC5"}, {"recognition", " 明天开会 "}};
    const auto inbound = parse_robot_message(audio, kClient);
    ASSERT_TRUE(inbound);
    ASSERT_EQ(inbound->attachments.size(), 1u);
    EXPECT_EQ(inbound->attachments[0].kind, AttachmentKind::Voice);
    EXPECT_EQ(inbound->attachments[0].transcript, u8"明天开会");
}

// 场景:分享文档卡片(interactiveCard)与机器人收不到的消息类型(unknownMsgType)。
// 期望:文档卡片变成“[文档] 标题 链接”文字;未知类型变成一个 Other 附件,让核心回复“暂不支持”。
TEST(DingTalkProtocol, CardsAndUnsupportedTypes) {
    auto card = private_text("");
    card.erase("text");
    card["msgtype"] = "interactiveCard";
    card["content"] = {{"title", "周报"}, {"biz_custom_action_url", "https://alidocs.dingtalk.com/i/x"}};
    const auto doc = parse_robot_message(card, kClient);
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc->text, "[文档] 周报 https://alidocs.dingtalk.com/i/x");

    auto unknown = private_text("");
    unknown.erase("text");
    unknown["msgtype"] = "unknownMsgType";
    unknown["content"] = {{"unknownMsgType", "用户发送了一条消息，机器人暂不支持接收。"}};
    const auto other = parse_robot_message(unknown, kClient);
    ASSERT_TRUE(other);
    EXPECT_TRUE(other->text.empty());
    ASSERT_EQ(other->attachments.size(), 1u);
    EXPECT_EQ(other->attachments[0].kind, AttachmentKind::Other);
}

// 场景:引用回复(text.isReplyMsg + repliedMsg,被引用内容是 JSON 字符串)。
// 期望:quote_text 为被引用消息的文字,正文不受影响。
TEST(DingTalkProtocol, QuoteReplyIsExtracted) {
    auto data = private_text("同意");
    data["text"]["isReplyMsg"] = true;
    data["text"]["repliedMsg"] = {{"msgType", "text"}, {"msgId", "old"},
                                  {"content", nlohmann::json{{"text", "上线时间定在周五"}}.dump()}};
    const auto inbound = parse_robot_message(data, kClient);
    ASSERT_TRUE(inbound);
    EXPECT_EQ(inbound->text, u8"同意");
    EXPECT_EQ(inbound->quote_text, u8"上线时间定在周五");
}

// 场景:消息缺 msgId、或者既无文字也无附件、或者 data 不是对象。
// 期望:缺 msgId 时用帧头 messageId 兜底;没有内容或格式不对返回 nullopt。
TEST(DingTalkProtocol, MissingFieldsAreHandled) {
    auto data = private_text("hi");
    data.erase("msgId");
    const auto fallback = parse_robot_message(data, kClient, "hdr-1");
    ASSERT_TRUE(fallback);
    EXPECT_EQ(fallback->message_id, "hdr-1");
    EXPECT_FALSE(parse_robot_message(data, kClient));

    EXPECT_FALSE(parse_robot_message(private_text("   "), kClient));
    EXPECT_FALSE(parse_robot_message(nlohmann::json::array(), kClient));
    auto no_sender = private_text("hi");
    no_sender.erase("senderStaffId");
    no_sender.erase("senderId");
    EXPECT_FALSE(parse_robot_message(no_sender, kClient));
}

// 场景:新版 OpenAPI 的 4xx 错误与旧版 OAPI / webhook 的 HTTP 200 + errcode 两种失败形态。
// 期望:两种都能解析出错误码与原因;HTTP 200 + errcode 0 不算失败。
TEST(DingTalkProtocol, ParsesBothErrorShapes) {
    const auto api = parse_api_error(400, R"({"code":"InvalidAuthentication","requestid":"R1","message":"不合法的access_token"})");
    EXPECT_EQ(api.code, "InvalidAuthentication");
    EXPECT_EQ(api.request_id, "R1");
    EXPECT_TRUE(is_token_error(api));

    ApiError oapi;
    EXPECT_TRUE(body_has_errcode(R"({"errcode":300001,"errmsg":"session 不存在"})", &oapi));
    EXPECT_EQ(oapi.errcode, 300001);
    EXPECT_EQ(oapi.message, "session 不存在");
    EXPECT_TRUE(is_webhook_gone(oapi));
    EXPECT_FALSE(body_has_errcode(R"({"errcode":0,"errmsg":"ok"})", nullptr));
    EXPECT_FALSE(body_has_errcode(R"({"processQueryKey":"k"})", nullptr));

    const auto bare = parse_api_error(502, "<html>bad gateway</html>");
    EXPECT_EQ(bare.message, "HTTP 502");
}

// 场景:各种限流与令牌失效的错误码。
// 期望:QpsLimit 归为短等待;send.too.fast / 130101 / 429 归为长等待;40014 与 HTTP 401 是令牌失效。
TEST(DingTalkProtocol, ClassifiesThrottleAndTokenErrors) {
    ApiError qps;
    qps.status = 403;
    qps.code = "Forbidden.AccessDenied.QpsLimitForApi";
    EXPECT_EQ(throttle_kind(qps), Throttle::Qps);
    ApiError fast;
    fast.status = 200;
    fast.errcode = 130101;
    EXPECT_EQ(throttle_kind(fast), Throttle::Rate);
    ApiError too_many;
    too_many.status = 429;
    EXPECT_EQ(throttle_kind(too_many), Throttle::Rate);
    ApiError normal;
    normal.status = 400;
    normal.code = "invalidParameter.msgParam.invalid";
    EXPECT_EQ(throttle_kind(normal), Throttle::None);
    ApiError oapi_token;
    oapi_token.errcode = 40014;
    EXPECT_TRUE(is_token_error(oapi_token));
}

// 场景:防 SSRF —— 回复用的 webhook 地址来自入站消息,可能被伪造。
// 期望:只接受 https 的 api/oapi.dingtalk.com;拒绝 http、子域名伪装、@ 伪装与带空白的地址;
// 配置的接入地址(测试里的本机假服务)作为额外前缀放行。
TEST(DingTalkProtocol, WebhookAllowlistBlocksForeignHosts) {
    EXPECT_TRUE(webhook_url_allowed("https://oapi.dingtalk.com/robot/sendBySession?session=x", {}));
    EXPECT_TRUE(webhook_url_allowed("https://api.dingtalk.com/v1.0/robot/x", {}));
    EXPECT_FALSE(webhook_url_allowed("http://oapi.dingtalk.com/robot/sendBySession", {}));
    EXPECT_FALSE(webhook_url_allowed("https://oapi.dingtalk.com.evil.com/robot", {}));
    EXPECT_FALSE(webhook_url_allowed("https://oapi.dingtalk.com@evil.com/robot", {}));
    EXPECT_FALSE(webhook_url_allowed("https://oapi.dingtalk.com/a b", {}));
    EXPECT_FALSE(webhook_url_allowed("http://127.0.0.1:9/robot", {}));
    EXPECT_TRUE(webhook_url_allowed("http://127.0.0.1:9/robot", {"http://127.0.0.1:9/"}));
    EXPECT_FALSE(webhook_url_allowed("http://127.0.0.1:99/robot", {"http://127.0.0.1:9"}));
}

// 场景:为 Markdown 消息生成通知栏标题。
// 期望:取第一行非空文字,去掉 # * > - 与反引号,截到 20 个字符(按码点,不切坏中文);全空时用默认标题。
TEST(DingTalkProtocol, MarkdownTitleUsesFirstLine) {
    EXPECT_EQ(markdown_title("\n## **部署结果**\n正文"), u8"部署结果");
    EXPECT_EQ(markdown_title("> - `make test` passed"), "make test passed");
    EXPECT_EQ(markdown_title(u8"一二三四五六七八九十一二三四五六七八九十多出来的"), u8"一二三四五六七八九十一二三四五六七八九十");
    EXPECT_EQ(markdown_title("  \n ** \n"), "ACECode");
}

// 场景:钉钉渲染器的两个怪癖(Hermes 实测):编号列表紧跟普通文字不渲染;缩进的代码围栏不识别。
// 期望:编号列表前补一个空行(相邻编号行之间不补);缩进围栏及其内容去掉围栏的缩进;围栏内的编号行不动。
TEST(DingTalkProtocol, NormalizesMarkdownQuirks) {
    EXPECT_EQ(normalize_markdown("步骤如下:\n1. 安装\n2. 运行"), "步骤如下:\n\n1. 安装\n2. 运行");
    EXPECT_EQ(normalize_markdown("- 项\n   ```bash\n   ls\n   ```\n"), "- 项\n```bash\nls\n```\n");
    EXPECT_EQ(normalize_markdown("```\ncode\n1. not a list\n```"), "```\ncode\n1. not a list\n```");
}

// 场景:按文件名 / MIME 决定媒体上传类型。
// 期望:常见图片为 image;amr/mp3/wav 为 voice;其余(含视频、PDF)为 file;扩展名小写且不带点。
TEST(DingTalkProtocol, UploadTypeAndExtension) {
    EXPECT_EQ(upload_type_for("image/png", "a.png"), "image");
    EXPECT_EQ(upload_type_for("", "Shot.JPG"), "image");
    EXPECT_EQ(upload_type_for("image/webp", "a.webp"), "file");
    EXPECT_EQ(upload_type_for("audio/amr", "v.amr"), "voice");
    EXPECT_EQ(upload_type_for("video/mp4", "clip.mp4"), "file");
    EXPECT_EQ(upload_type_for("application/pdf", "a.pdf"), "file");
    EXPECT_EQ(file_extension("报告.PDF"), "pdf");
    EXPECT_EQ(file_extension("README"), "");
    EXPECT_EQ(url_encode("a b+c/d"), "a%20b%2Bc%2Fd");
}

// 场景:把平台错误翻译给用户。
// 期望:缺发消息权限、IP 白名单、网络不通都给出带处理办法的中文一句话。
TEST(DingTalkProtocol, DescribesCommonErrors) {
    ApiError permission;
    permission.status = 403;
    permission.code = "Forbidden.AccessDenied.AccessTokenPermissionDenied";
    EXPECT_NE(describe_api_error(permission).find("qyapi_robot_sendmsg"), std::string::npos);
    ApiError ip;
    ip.status = 200;
    ip.errcode = 60020;
    EXPECT_NE(describe_api_error(ip).find(u8"白名单"), std::string::npos);
    ApiError offline;
    EXPECT_NE(describe_api_error(offline).find(u8"无法连接"), std::string::npos);
}

} // namespace
} // namespace acecode::im::dingtalk
