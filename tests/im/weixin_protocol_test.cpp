#include <gtest/gtest.h>

#include "im/weixin/weixin_media.hpp"
#include "im/weixin/weixin_protocol.hpp"
#include "platform/crypto/aes_ecb.hpp"

// im/weixin/weixin_protocol + weixin_media 的纯逻辑:请求头取值、响应包络判定、入站解析、
// 媒体密钥格式、出站条目、片段合并与去重。测试向量来自微信协议调研文档(hermes / 官方插件实测值)。

namespace acecode::im::weixin {
namespace {

constexpr const char* kBot = "e06c1ceea05e@im.bot";
constexpr const char* kUser = "o9cq800kum_owner@im.wechat";

std::string bytes_0_to_15() {
    std::string key;
    for (int i = 0; i < 16; ++i) key.push_back(static_cast<char>(i));
    return key;
}

nlohmann::json text_message(const std::string& text) {
    return {{"seq", 429},
            {"message_id", 7351234567890123456ULL},
            {"from_user_id", kUser},
            {"to_user_id", kBot},
            {"create_time_ms", 1759712345678LL},
            {"message_type", 1},
            {"message_state", 2},
            {"context_token", "AARzJWAFAAABAAAAAAAp2m3u7oE0x7V8Xw=="},
            {"item_list", nlohmann::json::array({{{"type", 1}, {"text_item", {{"text", text}}}}})}};
}

Inbound make_inbound(const std::string& sender, const std::string& text, const std::string& id) {
    Inbound inbound;
    inbound.address.platform = "weixin";
    inbound.address.account = kBot;
    inbound.address.chat = inbound.address.sender = sender;
    inbound.message_id = id;
    inbound.text = text;
    inbound.mentioned = true;
    return inbound;
}

// 场景:按 hermes 的 2.2.0 版本声明协议版本,并为每个请求生成 X-WECHAT-UIN。
// 期望:ClientVersion = (主<<16)|(次<<8)|修订(2.2.0 → 131584,2.4.9 → 132105);
// X-WECHAT-UIN 是十进制数字字符串的 base64(文档示例 3141592653 → MzE0MTU5MjY1Mw==)。
TEST(WeixinProtocol, ClientVersionAndUinHeader) {
    EXPECT_EQ(client_version_number("2.2.0"), 131584u);
    EXPECT_EQ(client_version_number("2.4.9"), 132105u);
    EXPECT_EQ(client_version_number("garbage"), 0u);
    EXPECT_EQ(wechat_uin(3141592653u), "MzE0MTU5MjY1Mw==");
    EXPECT_EQ(join_url("https://a.example/", "/ilink/bot/getupdates"), "https://a.example/ilink/bot/getupdates");
    EXPECT_EQ(url_encode("a b/c+d=e"), "a%20b%2Fc%2Bd%3De");
}

// 场景:平台的应用层错误以 HTTP 200 返回,且实测失效 token 的应答只有 errcode 没有 ret。
// 期望:{"errcode":-14} 判为失败且是“登录失效”;-2 + "unknown error" 也是失效(hermes #17228);
// -2 + "freq limit" 是限频;-2 + 参数错误既不是失效也不是限频;字段缺失按 0 视为成功;非 JSON 判失败。
TEST(WeixinProtocol, EnvelopeDetectsHttp200Errors) {
    const auto expired = parse_envelope(R"({"errcode":-14,"errmsg":"session timeout"})");
    EXPECT_TRUE(envelope_failed(expired));
    EXPECT_TRUE(is_session_expired(expired.ret, expired.errcode, expired.errmsg));

    EXPECT_TRUE(is_session_expired(-2, 0, "Unknown Error "));
    EXPECT_FALSE(is_rate_limited(-2, 0, "unknown error"));
    EXPECT_TRUE(is_rate_limited(-2, 0, "freq limit"));
    EXPECT_TRUE(is_rate_limited(0, -2, ""));
    EXPECT_FALSE(is_rate_limited(-2, 0, "parameter error"));
    EXPECT_FALSE(is_session_expired(-2, 0, "freq limit"));

    const auto ok = parse_envelope(R"({"msgs":[],"get_updates_buf":"x"})");
    EXPECT_FALSE(envelope_failed(ok));
    EXPECT_EQ(ok.body.value("get_updates_buf", ""), "x");
    EXPECT_TRUE(envelope_failed(parse_envelope("not json")));
    EXPECT_TRUE(envelope_failed(parse_envelope(R"({"ret":-1})")));
}

// 场景:收到用户私聊的一条文字,message_id 是超过 2^53 的 uint64。
// 期望:地址为 weixin / 机器人 id / 私聊 / chat = sender = 用户 id,私聊恒为点名;
// message_id 原样转成十进制(不丢精度);context_token 与发送时间被取出;回复定位只带消息 id。
TEST(WeixinProtocol, ParsesPrivateTextMessage) {
    const auto parsed = parse_message(text_message(u8"  你好  "), kBot);
    ASSERT_TRUE(parsed.inbound.has_value()) << parsed.drop_reason;
    const auto& inbound = *parsed.inbound;
    EXPECT_EQ(inbound.address.platform, "weixin");
    EXPECT_EQ(inbound.address.account, kBot);
    EXPECT_EQ(inbound.address.kind, ChatKind::Private);
    EXPECT_EQ(inbound.address.chat, kUser);
    EXPECT_EQ(inbound.address.sender, kUser);
    EXPECT_TRUE(inbound.mentioned);
    EXPECT_EQ(inbound.text, u8"你好");
    EXPECT_EQ(inbound.message_id, "7351234567890123456");
    EXPECT_EQ(inbound.reply_context.value("message_id", ""), "7351234567890123456");
    EXPECT_FALSE(inbound.reply_context.contains("context_token"));
    EXPECT_EQ(parsed.context_token, "AARzJWAFAAABAAAAAAAp2m3u7oE0x7V8Xw==");
    EXPECT_EQ(parsed.create_time_ms, 1759712345678LL);
    EXPECT_TRUE(parsed.from_user);
}

// 场景:机器人自己发出的消息、BOT 类型消息、没有发送人的消息、以及只有未知条目的空消息。
// 期望:前三者不算用户消息(from_user = false);空消息虽不交给上层,但仍算用户消息,
// 以便传输层记下它带来的最新 context_token。
TEST(WeixinProtocol, DropsBotOwnAndEmptyMessages) {
    auto own = text_message("x");
    own["from_user_id"] = kBot;
    EXPECT_FALSE(parse_message(own, kBot).from_user);

    auto bot_type = text_message("x");
    bot_type["message_type"] = 2;
    EXPECT_FALSE(parse_message(bot_type, kBot).from_user);

    auto anonymous = text_message("x");
    anonymous.erase("from_user_id");
    EXPECT_FALSE(parse_message(anonymous, kBot).from_user);

    auto empty = text_message("x");
    empty["item_list"] = nlohmann::json::array({{{"type", 11}}});
    const auto parsed = parse_message(empty, kBot);
    EXPECT_TRUE(parsed.from_user);
    EXPECT_FALSE(parsed.inbound.has_value());
    EXPECT_EQ(parsed.context_token, "AARzJWAFAAABAAAAAAAp2m3u7oE0x7V8Xw==");
}

// 场景:用户引用一条文字回复、引用一张图片提问、以及发来带平台转写文字的语音。
// 期望:引用文字进 quote_text(标题与原文不同时用“ | ”连接);被引用的图片作为附件附上,
// quote_text 标出“[图片]”;语音成为 Voice 附件,转写文字放在 transcript(由核心拼进正文)。
TEST(WeixinProtocol, QuotesAndVoiceTranscript) {
    auto quoted = text_message(u8"同意");
    quoted["item_list"][0]["ref_msg"] = {{"title", u8"摘要"},
                                         {"message_item", {{"type", 1}, {"text_item", {{"text", u8"原话"}}}}}};
    auto parsed = parse_message(quoted, kBot);
    ASSERT_TRUE(parsed.inbound);
    EXPECT_EQ(parsed.inbound->quote_text, u8"摘要 | 原话");
    EXPECT_TRUE(parsed.inbound->attachments.empty());

    auto quoted_image = text_message(u8"这张图是什么");
    quoted_image["item_list"][0]["ref_msg"] = {
        {"message_item", {{"type", 2}, {"image_item", {{"aeskey", "00112233445566778899aabbccddeeff"},
                                                       {"media", {{"encrypt_query_param", "Q1"}}}}}}}};
    parsed = parse_message(quoted_image, kBot);
    ASSERT_TRUE(parsed.inbound);
    EXPECT_EQ(parsed.inbound->quote_text, u8"[图片]");
    ASSERT_EQ(parsed.inbound->attachments.size(), 1u);
    EXPECT_EQ(parsed.inbound->attachments[0].kind, AttachmentKind::Image);

    auto voice = text_message("");
    voice["item_list"] = nlohmann::json::array(
        {{{"type", 3}, {"voice_item", {{"text", u8"明天开会"}, {"encode_type", 6},
                                       {"media", {{"encrypt_query_param", "V1"}, {"aes_key", "ABEiM0RVZneImaq7zN3u/w=="}}}}}}});
    parsed = parse_message(voice, kBot);
    ASSERT_TRUE(parsed.inbound);
    EXPECT_TRUE(parsed.inbound->text.empty());
    ASSERT_EQ(parsed.inbound->attachments.size(), 1u);
    EXPECT_EQ(parsed.inbound->attachments[0].kind, AttachmentKind::Voice);
    EXPECT_EQ(parsed.inbound->attachments[0].transcript, u8"明天开会");
}

// 场景:收到图片(image_item.aeskey 十六进制密钥)、文件(media.aes_key 是“十六进制文本的 base64”)、
// 视频(media.aes_key 是原始 16 字节的 base64)以及密钥格式坏掉的文件。
// 期望:附件类型、文件名、MIME、大小(file_item.len 是十进制字符串)正确;remote_ref 能还原出
// 下载参数与同一把 16 字节密钥;坏密钥被标记,下载时会明确报错而不是把密文当明文保存。
TEST(WeixinProtocol, MediaAttachmentsCarryDownloadRef) {
    auto message = text_message("");
    message["item_list"] = nlohmann::json::array({
        {{"type", 2}, {"image_item", {{"aeskey", "00112233445566778899aabbccddeeff"},
                                      {"media", {{"encrypt_query_param", "IMG"}}}}}},
        {{"type", 4}, {"file_item", {{"file_name", u8"报告.pdf"}, {"len", "12345"},
                                     {"media", {{"encrypt_query_param", "FILE"},
                                                {"aes_key", "MDAxMTIyMzM0NDU1NjY3Nzg4OTlhYWJiY2NkZGVlZmY="}}}}}},
        {{"type", 5}, {"video_item", {{"media", {{"full_url", "https://novac2c.cdn.weixin.qq.com/c2c/download?x=1"},
                                                 {"aes_key", "ABEiM0RVZneImaq7zN3u/w=="}}}}}},
        {{"type", 4}, {"file_item", {{"file_name", "bad.bin"},
                                     {"media", {{"encrypt_query_param", "BAD"}, {"aes_key", "AAAA"}}}}}},
    });
    const auto parsed = parse_message(message, kBot);
    ASSERT_TRUE(parsed.inbound);
    const auto& files = parsed.inbound->attachments;
    ASSERT_EQ(files.size(), 4u);
    const auto expected_key = *hex_decode("00112233445566778899aabbccddeeff");

    EXPECT_EQ(files[0].kind, AttachmentKind::Image);
    auto ref = decode_media_ref(files[0].remote_ref);
    ASSERT_TRUE(ref);
    EXPECT_EQ(ref->query_param, "IMG");
    EXPECT_EQ(ref->aes_key, expected_key);
    EXPECT_EQ(ref->kind, "image");

    EXPECT_EQ(files[1].kind, AttachmentKind::File);
    EXPECT_EQ(files[1].name, u8"报告.pdf");
    EXPECT_EQ(files[1].mime_type, "application/pdf");
    EXPECT_EQ(files[1].size, 12345u);
    ref = decode_media_ref(files[1].remote_ref);
    ASSERT_TRUE(ref);
    EXPECT_EQ(ref->aes_key, expected_key);

    EXPECT_EQ(files[2].kind, AttachmentKind::Video);
    ref = decode_media_ref(files[2].remote_ref);
    ASSERT_TRUE(ref);
    EXPECT_TRUE(ref->query_param.empty());
    EXPECT_EQ(ref->full_url, "https://novac2c.cdn.weixin.qq.com/c2c/download?x=1");
    EXPECT_EQ(ref->aes_key, expected_key);

    ref = decode_media_ref(files[3].remote_ref);
    ASSERT_TRUE(ref);
    EXPECT_TRUE(ref->key_invalid);
}

// 场景:平台下发的媒体密钥有两种编码,出站时还要按平台要求编码密钥。
// 期望:base64(16 字节)与 base64(32 位十六进制文本)都还原成同一把密钥,其它长度拒绝;
// 出站 aes_key = base64(小写十六进制文本)(文档示例 00..0f → MDAwMTAy…MGY=),
// 用 base64(原始字节) 会让对方看到灰色方块。
TEST(WeixinProtocol, AesKeyFormats) {
    const auto expected = *hex_decode("00112233445566778899aabbccddeeff");
    EXPECT_EQ(parse_aes_key("ABEiM0RVZneImaq7zN3u/w=="), expected);
    EXPECT_EQ(parse_aes_key("MDAxMTIyMzM0NDU1NjY3Nzg4OTlhYWJiY2NkZGVlZmY="), expected);
    EXPECT_FALSE(parse_aes_key("AAAA").has_value());
    EXPECT_FALSE(parse_aes_key("!!").has_value());
    EXPECT_EQ(outbound_aes_key(bytes_0_to_15()), "MDAwMTAyMDMwNDA1MDYwNzA4MDkwYTBiMGMwZDBlMGY=");
}

// 场景:解密 CDN 下载的媒体;平台偶尔给出填充不规范的密文。
// 期望:FIPS-197 的 AES-128 标准向量能解出原文(末字节 0xff 不是合法填充,宽松规则下原样保留);
// 正常加密的内容往返一致;长度不是 16 的倍数的密文报错;宽松去填充只去合法的填充。
TEST(WeixinProtocol, DecryptsMediaWithLenientPadding) {
    const auto key = bytes_0_to_15();
    const auto ciphertext = *hex_decode("69c4e0d86a7b0430d8cdb78070b4c55a");
    std::string plaintext, error;
    ASSERT_TRUE(decrypt_media(key, ciphertext, plaintext, &error)) << error;
    EXPECT_EQ(plaintext, *hex_decode("00112233445566778899aabbccddeeff"));

    std::string encrypted;
    ASSERT_TRUE(platform::aes_128_ecb_encrypt(key, u8"微信图片内容", encrypted, nullptr));
    ASSERT_TRUE(decrypt_media(key, encrypted, plaintext, &error)) << error;
    EXPECT_EQ(plaintext, u8"微信图片内容");

    EXPECT_FALSE(decrypt_media(key, std::string(15, 'x'), plaintext, &error));
    EXPECT_FALSE(error.empty());

    EXPECT_EQ(strip_pkcs7_lenient(std::string("abc") + std::string(3, '\x03')), "abc");
    EXPECT_EQ(strip_pkcs7_lenient(std::string("abc\x02\x03")), std::string("abc\x02\x03"));
    EXPECT_EQ(strip_pkcs7_lenient(std::string("abc\x00", 4)), std::string("abc\x00", 4));
}

// 场景:回传文件时按 MIME 选择上传类型,并构造 sendmessage 请求体。
// 期望:图片(SVG 除外)→ 1,视频 → 2,音频与其它 → 3(文件);文字消息 from_user_id 为空、
// message_type 2、message_state 2,没有 context_token 时不带该字段;文件条目的 len 是十进制字符串。
TEST(WeixinProtocol, UploadTypesAndMessageBodies) {
    EXPECT_EQ(upload_media_type("image/png", "a.png"), UploadMediaType::Image);
    EXPECT_EQ(upload_media_type("", "photo.JPG"), UploadMediaType::Image);
    EXPECT_EQ(upload_media_type("image/svg+xml", "a.svg"), UploadMediaType::File);
    EXPECT_EQ(upload_media_type("video/mp4", "a.mp4"), UploadMediaType::Video);
    EXPECT_EQ(upload_media_type("audio/silk", "v.silk"), UploadMediaType::File);
    EXPECT_EQ(upload_media_type("application/octet-stream", "r.pdf"), UploadMediaType::File);

    const auto body = build_send_body(kUser, "cid-1", text_item(u8"回复"), "");
    const auto& msg = body["msg"];
    EXPECT_EQ(msg.value("from_user_id", "?"), "");
    EXPECT_EQ(msg.value("to_user_id", ""), kUser);
    EXPECT_EQ(msg.value("client_id", ""), "cid-1");
    EXPECT_EQ(msg.value("message_type", 0), 2);
    EXPECT_EQ(msg.value("message_state", 0), 2);
    EXPECT_FALSE(msg.contains("context_token"));
    EXPECT_EQ(msg["item_list"][0]["text_item"].value("text", ""), u8"回复");
    EXPECT_EQ(build_send_body(kUser, "c", text_item("x"), "tok")["msg"].value("context_token", ""), "tok");

    const auto file = media_item(UploadMediaType::File, "ENC", bytes_0_to_15(), 32, 20, "md5", "a.txt");
    EXPECT_EQ(file.value("type", 0), 4);
    EXPECT_EQ(file["file_item"].value("len", ""), "20");
    EXPECT_EQ(file["file_item"]["media"].value("aes_key", ""), "MDAwMTAyMDMwNDA1MDYwNzA4MDkwYTBiMGMwZDBlMGY=");
    EXPECT_EQ(file["file_item"]["media"].value("encrypt_type", 0), 1);
    const auto image = media_item(UploadMediaType::Image, "ENC", bytes_0_to_15(), 48, 40, "md5", "a.png");
    EXPECT_EQ(image.value("type", 0), 2);
    EXPECT_EQ(image["image_item"].value("mid_size", 0), 48);
}

// 场景:拼 CDN 上传 / 下载地址;入站媒体只给了 full_url。
// 期望:参数全部百分号编码;full_url 只接受微信自家域名或配置的 CDN 地址,
// 拒绝其它主机、带用户名的地址与非 http(s) 协议(防止被诱导请求内网)。
TEST(WeixinProtocol, CdnUrlsAndHostAllowlist) {
    EXPECT_EQ(cdn_download_url("https://novac2c.cdn.weixin.qq.com/c2c", "a+b/c"),
              "https://novac2c.cdn.weixin.qq.com/c2c/download?encrypted_query_param=a%2Bb%2Fc");
    EXPECT_EQ(cdn_upload_url("https://novac2c.cdn.weixin.qq.com/c2c/", "p=1", "ab"),
              "https://novac2c.cdn.weixin.qq.com/c2c/upload?encrypted_query_param=p%3D1&filekey=ab");
    const std::string cdn = kCdnBase;
    EXPECT_TRUE(media_host_allowed("https://novac2c.cdn.weixin.qq.com/c2c/download?x=1", cdn));
    EXPECT_TRUE(media_host_allowed("https://MMBIZ.QPIC.CN/a.jpg", cdn));
    EXPECT_FALSE(media_host_allowed("https://evil.example/a.jpg", cdn));
    EXPECT_FALSE(media_host_allowed("https://evil@novac2c.cdn.weixin.qq.com/a", cdn));
    EXPECT_FALSE(media_host_allowed("file:///etc/passwd", cdn));
    EXPECT_FALSE(media_host_allowed("http://127.0.0.1:8080/c2c/x", cdn));
    EXPECT_TRUE(media_host_allowed("http://127.0.0.1:8080/c2c/x", "http://127.0.0.1:8080/c2c"));
    EXPECT_FALSE(media_host_allowed("http://127.0.0.1:9090/c2c/x", "http://127.0.0.1:8080/c2c"));
}

// 场景:用户粘贴一段约 2500 字的长文,平台把它拆成 1900 字 + 600 字两条依次投递。
// 期望:第一段(达到 1800 字阈值)先暂存不交付;第二段到来时合并成一条交付,沿用第一段的消息 id;
// 普通短消息不等待;长片段等不到后续时到期原样交付;暂存期间来了媒体消息则先交付暂存内容再交付媒体。
TEST(WeixinProtocol, FragmentMergerJoinsSplitMessages) {
    using Clock = FragmentMerger::Clock;
    MergeLimits limits;
    limits.split_threshold = 1800;
    limits.wait = std::chrono::milliseconds(5000);
    FragmentMerger merger(limits);
    const auto t0 = Clock::now();
    const std::string first(1900, 'a');

    EXPECT_TRUE(merger.push(make_inbound(kUser, first, "m1"), t0).empty());
    auto ready = merger.push(make_inbound(kUser, "tail", "m2"), t0 + std::chrono::milliseconds(300));
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0].text, first + "\ntail");
    EXPECT_EQ(ready[0].message_id, "m1");
    EXPECT_TRUE(merger.empty());

    ready = merger.push(make_inbound(kUser, "short", "m3"), t0);
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0].text, "short");

    EXPECT_TRUE(merger.push(make_inbound(kUser, first, "m4"), t0).empty());
    EXPECT_TRUE(merger.take_due(t0 + std::chrono::milliseconds(4000)).empty());
    ready = merger.take_due(t0 + std::chrono::milliseconds(5000));
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0].message_id, "m4");

    EXPECT_TRUE(merger.push(make_inbound(kUser, first, "m5"), t0).empty());
    auto media = make_inbound(kUser, "", "m6");
    media.attachments.push_back(Attachment{AttachmentKind::Image});
    ready = merger.push(std::move(media), t0);
    ASSERT_EQ(ready.size(), 2u);
    EXPECT_EQ(ready[0].message_id, "m5");
    EXPECT_EQ(ready[1].message_id, "m6");
}

// 场景:平台重复投递同一 message_id;又换了一个 message_id 重发同一内容(同一发送时间);
// 用户隔了一会儿又发了一次同样的“继续”(发送时间不同)。
// 期望:前两种只处理一次;第三种照常处理。没有发送时间时,只在 10 秒窗口内按内容去重。
TEST(WeixinProtocol, DeduperDropsRepeats) {
    using Clock = InboundDeduper::Clock;
    InboundDeduper dedupe(1000, std::chrono::seconds(10));
    const auto now = Clock::now();
    EXPECT_FALSE(dedupe.seen_id("1"));
    EXPECT_TRUE(dedupe.seen_id("1"));
    EXPECT_FALSE(dedupe.seen_id(""));

    EXPECT_FALSE(dedupe.seen_content(kUser, u8"继续", 1000, now));
    EXPECT_TRUE(dedupe.seen_content(kUser, u8"继续", 1000, now));
    EXPECT_FALSE(dedupe.seen_content(kUser, u8"继续", 2000, now));

    EXPECT_FALSE(dedupe.seen_content(kUser, "x", 0, now));
    EXPECT_TRUE(dedupe.seen_content(kUser, "x", 0, now + std::chrono::seconds(5)));
    EXPECT_FALSE(dedupe.seen_content(kUser, "x", 0, now + std::chrono::seconds(30)));
    EXPECT_FALSE(dedupe.seen_content(kUser, "", 0, now));
    EXPECT_FALSE(dedupe.seen_content(kUser, "", 0, now));
}

} // namespace
} // namespace acecode::im::weixin
