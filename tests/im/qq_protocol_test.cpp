#include <gtest/gtest.h>

#include "im/qqbot/qq_protocol.hpp"

// im/qqbot/qq_protocol:网关关闭码分类、消息事件解析、OpenAPI 错误解析。

namespace acecode::im::qqbot {
namespace {

// 场景:网关以各种关闭码断开。
// 期望:令牌失效先刷新;会话类错误重新登录;频控单独处理;封禁、下线/仅沙箱为致命错误;
// 其余普通断线按恢复会话处理。
TEST(QqProtocol, ClassifiesGatewayCloseCodes) {
    EXPECT_EQ(classify_close(4004).action, CloseAction::RefreshToken);
    for (int code : {4006, 4007, 4009, 4900, 4913, 4001})
        EXPECT_EQ(classify_close(code).action, CloseAction::Identify) << code;
    EXPECT_EQ(classify_close(4008).action, CloseAction::RateLimited);
    EXPECT_EQ(classify_close(4914).action, CloseAction::Fatal);
    EXPECT_EQ(classify_close(4915).action, CloseAction::Fatal);
    EXPECT_FALSE(classify_close(4915).reason.empty());
    for (int code : {1000, 1001, 1006, 4000})
        EXPECT_EQ(classify_close(code).action, CloseAction::Resume) << code;
}

// 场景:收到一条单聊消息,正文两端带空白。
// 期望:地址为私聊(对方 openid 同时是 chat 与 sender),视为点名,正文去空白;
// 回复上下文里记录 msg_id、scope=c2c 与接收时间。
TEST(QqProtocol, ParsesC2CMessage) {
    const auto inbound = parse_message_event(
        "C2C_MESSAGE_CREATE",
        {{"id", "M1"}, {"content", "  你好  "}, {"author", {{"user_openid", "U1"}}}}, "APP", 1234);
    ASSERT_TRUE(inbound);
    EXPECT_EQ(inbound->address.platform, "qq");
    EXPECT_EQ(inbound->address.account, "APP");
    EXPECT_EQ(inbound->address.kind, ChatKind::Private);
    EXPECT_EQ(inbound->address.chat, "U1");
    EXPECT_EQ(inbound->address.sender, "U1");
    EXPECT_TRUE(inbound->mentioned);
    EXPECT_EQ(inbound->text, u8"你好");
    EXPECT_EQ(inbound->reply_context.value("msg_id", ""), "M1");
    EXPECT_EQ(inbound->reply_context.value("scope", ""), "c2c");
    EXPECT_EQ(inbound->reply_context.value("received_at_ms", 0), 1234);
}

// 场景:群里 @机器人 的消息,以及群主开启“接收全部消息”后推来的普通群消息。
// 期望:前者按“群 + 成员”定位且视为点名;后者同样能解析,但标记为未点名(核心会忽略)。
TEST(QqProtocol, ParsesGroupMessagesWithMentionFlag) {
    const nlohmann::json d{{"id", "M2"}, {"content", " 帮我看看 "}, {"group_openid", "G1"},
                           {"author", {{"member_openid", "MB1"}}}};
    const auto at = parse_message_event("GROUP_AT_MESSAGE_CREATE", d, "APP", 0);
    ASSERT_TRUE(at);
    EXPECT_EQ(at->address.kind, ChatKind::Group);
    EXPECT_EQ(at->address.chat, "G1");
    EXPECT_EQ(at->address.sender, "MB1");
    EXPECT_TRUE(at->mentioned);
    EXPECT_EQ(at->reply_context.value("scope", ""), "group");
    const auto all = parse_message_event("GROUP_MESSAGE_CREATE", d, "APP", 0);
    ASSERT_TRUE(all);
    EXPECT_FALSE(all->mentioned);
}

// 场景:消息带图片、带识别文字的语音、普通文件三种附件,地址以 // 开头。
// 期望:类型分别识别为图片、语音、文件;语音保留识别文字;省略协议的 // 地址补成 https。
TEST(QqProtocol, ParsesAttachmentsAndVoiceTranscript) {
    const nlohmann::json d{
        {"id", "M3"}, {"content", ""}, {"author", {{"user_openid", "U1"}}},
        {"attachments", nlohmann::json::array({
            {{"content_type", "image/png"}, {"filename", "a.png"}, {"url", "//cdn.example/a"}, {"size", 10}},
            {{"content_type", "voice"}, {"filename", "v.amr"}, {"url", "https://cdn.example/v"},
             {"asr_refer_text", " 明天开会 "}},
            {{"content_type", "file"}, {"filename", "r.pdf"}, {"url", "http://cdn.example/r"}}})}};
    const auto inbound = parse_message_event("C2C_MESSAGE_CREATE", d, "APP", 0);
    ASSERT_TRUE(inbound);
    ASSERT_EQ(inbound->attachments.size(), 3u);
    EXPECT_EQ(inbound->attachments[0].kind, AttachmentKind::Image);
    EXPECT_EQ(inbound->attachments[0].remote_ref, "https://cdn.example/a");
    EXPECT_EQ(inbound->attachments[0].size, 10u);
    EXPECT_EQ(inbound->attachments[1].kind, AttachmentKind::Voice);
    EXPECT_EQ(inbound->attachments[1].transcript, u8"明天开会");
    EXPECT_EQ(inbound->attachments[2].kind, AttachmentKind::File);
    EXPECT_EQ(inbound->attachments[2].remote_ref, "http://cdn.example/r");
}

// 场景:频道消息、READY 之类的非消息事件、没有正文也没有附件的消息、缺少发送者的消息。
// 期望:都不产生入站消息。
TEST(QqProtocol, IgnoresUnsupportedOrEmptyEvents) {
    const nlohmann::json ok{{"id", "M"}, {"content", "x"}, {"author", {{"user_openid", "U"}}}};
    EXPECT_FALSE(parse_message_event("AT_MESSAGE_CREATE", ok, "APP", 0));
    EXPECT_FALSE(parse_message_event("READY", ok, "APP", 0));
    EXPECT_FALSE(parse_message_event("C2C_MESSAGE_CREATE",
                                     {{"id", "M"}, {"content", "  "}, {"author", {{"user_openid", "U"}}}}, "APP", 0));
    EXPECT_FALSE(parse_message_event("C2C_MESSAGE_CREATE", {{"id", "M"}, {"content", "x"}}, "APP", 0));
}

// 场景:OpenAPI 返回 {"code","message"} 错误体,或返回无法解析的内容。
// 期望:读出错误码与原因;解析不了时用 HTTP 状态码兜底。
TEST(QqProtocol, ParsesApiErrors) {
    const auto error = parse_api_error(400, R"({"code":40034,"message":"msg_id expired"})");
    EXPECT_EQ(error.status, 400);
    EXPECT_EQ(error.code, 40034);
    EXPECT_EQ(error.message, "msg_id expired");
    EXPECT_EQ(parse_api_error(502, "<html>").message, "HTTP 502");
}

// 场景:按文件类型选择上传时的 file_type。
// 期望:图片 1、mp4 视频 2,其余(含音频与文档)一律按普通文件 4。
TEST(QqProtocol, MapsUploadFileTypes) {
    EXPECT_EQ(file_type_for("image/png", "a.png"), kFileTypeImage);
    EXPECT_EQ(file_type_for("", "photo.JPG"), kFileTypeImage);
    EXPECT_EQ(file_type_for("video/mp4", "v.mp4"), kFileTypeVideo);
    EXPECT_EQ(file_type_for("audio/mpeg", "a.mp3"), kFileTypeFile);
    EXPECT_EQ(file_type_for("application/pdf", "r.pdf"), kFileTypeFile);
}

} // namespace
} // namespace acecode::im::qqbot
