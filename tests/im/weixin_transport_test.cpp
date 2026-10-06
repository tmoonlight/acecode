#include <gtest/gtest.h>

#include "im/text_chunk.hpp"
#include "im/weixin/weixin_media.hpp"
#include "im/weixin/weixin_transport.hpp"
#include "platform/crypto/aes_ecb.hpp"
#include "platform/crypto/digest.hpp"
#include "test_support/im/fake_weixin_server.hpp"
#include "utils/base64.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

// im/weixin/weixin_transport:微信 iLink 传输层端到端测试(本机假 iLink 服务与假 CDN)。
// 覆盖:协议头与 base_info、长轮询收消息与游标持久化、从持久化游标继续、登录失效停止重试、
// HTTP 200 错误后重试、去重、长消息合并、context_token 的使用与失效重发、限频重发、
// 正在输入、文件上传与下载解密、及时停机、长轮询挂起时即显示已连接。

namespace acecode::im::weixin {
namespace {

constexpr const char* kBot = "e06c1ceea05e@im.bot";
constexpr const char* kUser = "o9cq800kum_owner@im.wechat";

WeixinTransportOptions local_options(const test::FakeWeixinServer& server) {
    WeixinTransportOptions options;
    options.api.token = server.token();
    options.api.base_url = server.base();
    options.api.cdn_base = server.cdn_base();
    options.api.use_proxy = false;
    options.bot_id = kBot;
    options.poll_timeout = std::chrono::milliseconds(300);
    options.poll_margin = std::chrono::seconds(2);
    options.connect_grace = std::chrono::milliseconds(200);
    options.backoff = {std::chrono::milliseconds(50)};
    options.chunk_delay = std::chrono::milliseconds(10);
    options.send_retry_delay = std::chrono::milliseconds(20);
    options.rate_limit_delay = std::chrono::milliseconds(50);
    options.typing_interval = std::chrono::milliseconds(100);
    options.housekeeping_tick = std::chrono::milliseconds(20);
    options.merge.wait = std::chrono::milliseconds(1000);
    return options;
}

struct Recorder {
    std::mutex mu;
    std::vector<Inbound> inbound;
    std::vector<LinkState> states;
    TransportCallbacks callbacks() {
        TransportCallbacks cb;
        cb.on_inbound = [this](Inbound in) {
            std::lock_guard<std::mutex> lock(mu);
            inbound.push_back(std::move(in));
        };
        cb.on_status = [this](const TransportStatus& status) {
            std::lock_guard<std::mutex> lock(mu);
            states.push_back(status.state);
        };
        return cb;
    }
    std::size_t count() {
        std::lock_guard<std::mutex> lock(mu);
        return inbound.size();
    }
    Inbound at(std::size_t i) {
        std::lock_guard<std::mutex> lock(mu);
        return inbound.at(i);
    }
    bool saw(LinkState state) {
        std::lock_guard<std::mutex> lock(mu);
        return std::find(states.begin(), states.end(), state) != states.end();
    }
};

Address user_address() {
    Address to;
    to.platform = "weixin";
    to.account = kBot;
    to.chat = to.sender = kUser;
    return to;
}

bool wait_state(WeixinTransport& transport, LinkState state) {
    return test::FakeWeixinServer::wait_until([&] { return transport.status().state == state; });
}

std::filesystem::path temp_dir(const char* name) {
    const auto dir = std::filesystem::path(testing::TempDir()) / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// 场景:开启微信通道后,用户私聊发来一条消息(message_id 是超过 2^53 的 uint64)。
// 期望:状态变为已连接,账号为机器人 id;收到入站消息且 id 不丢精度;新游标与该用户的
// context_token 都通过回调交给调用方持久化;getupdates 请求带齐协议头(Bearer token、
// AuthorizationType、iLink-App-Id、ClientVersion 131584、每次随机的 X-WECHAT-UIN = base64(数字))
// 与 base_info.channel_version = 2.2.0。
TEST(WeixinTransport, ConnectsReceivesAndPersistsCursor) {
    test::FakeWeixinServer server;
    auto options = local_options(server);
    std::mutex mu;
    std::string persisted;
    std::map<std::string, std::string> tokens;
    options.on_cursor = [&](const std::string& cursor) {
        std::lock_guard<std::mutex> lock(mu);
        persisted = cursor;
    };
    options.on_context_token = [&](const std::string& peer, const std::string& token) {
        std::lock_guard<std::mutex> lock(mu);
        tokens[peer] = token;
    };
    WeixinTransport transport(options);
    Recorder recorder;
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    EXPECT_EQ(transport.status().account, kBot);

    server.push_text(kUser, u8"你好", 7351234567890123456ULL, "ctx-A");
    ASSERT_TRUE(test::FakeWeixinServer::wait_until([&] { return recorder.count() == 1; }));
    const auto inbound = recorder.at(0);
    EXPECT_EQ(inbound.text, u8"你好");
    EXPECT_EQ(inbound.message_id, "7351234567890123456");
    EXPECT_EQ(inbound.address.platform, "weixin");
    EXPECT_EQ(inbound.address.account, kBot);
    EXPECT_EQ(inbound.address.chat, kUser);
    EXPECT_TRUE(inbound.mentioned);
    ASSERT_TRUE(test::FakeWeixinServer::wait_until([&] {
        std::lock_guard<std::mutex> lock(mu);
        return persisted == "cur-1" && tokens[kUser] == "ctx-A";
    }));
    EXPECT_EQ(transport.cursor(), "cur-1");

    const auto polls = server.calls_to("getupdates");
    ASSERT_FALSE(polls.empty());
    const auto& headers = polls[0].headers;
    EXPECT_EQ(headers.at("Authorization"), "Bearer " + server.token());
    EXPECT_EQ(headers.at("AuthorizationType"), "ilink_bot_token");
    EXPECT_EQ(headers.at("iLink-App-Id"), "bot");
    EXPECT_EQ(headers.at("iLink-App-ClientVersion"), "131584");
    const auto uin = base64_decode(headers.at("X-WECHAT-UIN"));
    ASSERT_TRUE(uin.has_value());
    EXPECT_FALSE(uin->empty());
    EXPECT_EQ(uin->find_first_not_of("0123456789"), std::string::npos);
    EXPECT_EQ(polls[0].body["base_info"].value("channel_version", ""), "2.2.0");
    EXPECT_TRUE(polls[0].body.contains("get_updates_buf"));
    transport.stop();
}

// 场景:重启时把持久化的游标交给传输层,服务端还留着之前处理过的消息。
// 期望:第一次 getupdates 就带上该游标,旧消息不会再交给上层。
TEST(WeixinTransport, ResumesFromPersistedCursor) {
    test::FakeWeixinServer server;
    server.push_text(kUser, "old", 1);  // seq 1
    server.push_text(kUser, "new", 2);  // seq 2
    auto options = local_options(server);
    options.initial_cursor = "cur-1";
    WeixinTransport transport(options);
    Recorder recorder;
    transport.start(recorder.callbacks());
    ASSERT_TRUE(test::FakeWeixinServer::wait_until([&] { return recorder.count() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(recorder.count(), 1u);
    EXPECT_EQ(recorder.at(0).text, "new");
    EXPECT_EQ(server.calls_to("getupdates").at(0).body.value("get_updates_buf", ""), "cur-1");
}

// 场景:bot_token 已失效,平台对 getupdates 返回 HTTP 200 + {"errcode":-14,"errmsg":"session timeout"}。
// 期望:状态 Failed 且停止重试,原因提示到设置页重新扫码,不含 token;之后不再继续轮询。
TEST(WeixinTransport, SessionExpiredStopsRetrying) {
    test::FakeWeixinServer server;
    auto options = local_options(server);
    options.api.token = "stale-token-ZYXWVUTSRQPONMLK";
    WeixinTransport transport(options);
    Recorder recorder;
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Failed));
    const auto status = transport.status();
    EXPECT_TRUE(status.retry_stopped);
    EXPECT_EQ(status.detail, kSessionExpiredText);
    EXPECT_EQ(status.detail.find("stale-token"), std::string::npos);
    const auto polls = server.calls_to("getupdates").size();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(server.calls_to("getupdates").size(), polls);
    EXPECT_FALSE(recorder.saw(LinkState::Connected));
}

// 场景:平台第一次 getupdates 返回 HTTP 200 + {"ret":-1,"errmsg":"system busy"}(临时错误)。
// 期望:进入重试状态并按退避重试,随后恢复为已连接并正常收到消息。
TEST(WeixinTransport, RetriesAfterHttp200Error) {
    test::FakeWeixinServer server;
    std::atomic<int> polls{0};
    server.override = [&](const test::FakeWeixinServer::Call& call) -> nlohmann::json {
        if (call.endpoint == "getupdates" && ++polls == 1) return {{"ret", -1}, {"errmsg", "system busy"}};
        return nullptr;
    };
    WeixinTransport transport(local_options(server));
    Recorder recorder;
    transport.start(recorder.callbacks());
    ASSERT_TRUE(test::FakeWeixinServer::wait_until([&] { return recorder.saw(LinkState::Retrying); }));
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    server.push_text(kUser, "hello", 10);
    ASSERT_TRUE(test::FakeWeixinServer::wait_until([&] { return recorder.count() == 1; }));
}

// 场景:平台重复投递同一 message_id;又换了个 message_id 重发同一内容(发送时间相同,hermes #16182);
// 之后用户发来一条新消息。
// 期望:重复的两条都被忽略,只交付第一条与新消息。
TEST(WeixinTransport, DedupesRepeatedMessages) {
    test::FakeWeixinServer server;
    auto first = test::FakeWeixinServer::text_message(kUser, u8"执行一下", 100);
    first["create_time_ms"] = 5555;
    auto resent = first;
    resent["message_id"] = 101;
    server.push_message(first);
    server.push_message(first);
    server.push_message(resent);
    server.push_text(kUser, u8"新消息", 102);
    WeixinTransport transport(local_options(server));
    Recorder recorder;
    transport.start(recorder.callbacks());
    ASSERT_TRUE(test::FakeWeixinServer::wait_until([&] { return recorder.count() == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_EQ(recorder.count(), 2u);
    EXPECT_EQ(recorder.at(0).message_id, "100");
    EXPECT_EQ(recorder.at(1).text, u8"新消息");
}

// 场景:用户粘贴一段长文,平台按约 2048 字拆成 1900 字 + “尾巴”两条投递;另一段 1900 字的长文没有后续。
// 期望:前者合并为一条交付(换行连接,消息 id 沿用第一段);后者等待期满后原样交付。
TEST(WeixinTransport, MergesSplitLongMessage) {
    test::FakeWeixinServer server;
    std::string longer;
    for (int i = 0; i < 1900; ++i) longer += u8"长";
    WeixinTransport transport(local_options(server));
    Recorder recorder;
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    server.push_text(kUser, longer, 1);
    server.push_text(kUser, u8"尾巴", 2);
    ASSERT_TRUE(test::FakeWeixinServer::wait_until([&] { return recorder.count() == 1; }));
    EXPECT_EQ(recorder.at(0).text, longer + u8"\n尾巴");
    EXPECT_EQ(recorder.at(0).message_id, "1");

    server.push_text(kUser, longer, 3);
    ASSERT_TRUE(test::FakeWeixinServer::wait_until([&] { return recorder.count() == 2; }));
    EXPECT_EQ(recorder.at(1).message_id, "3");
}

// 场景:用户发来消息后,助手回复一段 Markdown,以及一段 4500 个汉字的长回答。
// 期望:回复转成纯文本并带上该用户最新的 context_token、from_user_id 为空、message_type 2;
// 长回答按 2000 字(码点)切成 3 段依次发送,内容拼起来与原文一致,每段 client_id 不同。
TEST(WeixinTransport, SendsPlainTextWithContextTokenAndChunks) {
    test::FakeWeixinServer server;
    WeixinTransport transport(local_options(server));
    Recorder recorder;
    transport.start(recorder.callbacks());
    server.push_text(kUser, "hi", 1, "ctx-B");
    ASSERT_TRUE(test::FakeWeixinServer::wait_until([&] { return recorder.count() == 1; }));
    const auto inbound = recorder.at(0);
    EXPECT_EQ(transport.send_text(inbound.address, u8"**粗体** 文字", inbound.reply_context).outcome,
              SendOutcome::Sent);
    auto sends = server.calls_to("sendmessage");
    ASSERT_EQ(sends.size(), 1u);
    const auto& msg = sends[0].body["msg"];
    EXPECT_EQ(msg.value("to_user_id", ""), kUser);
    EXPECT_EQ(msg.value("from_user_id", "?"), "");
    EXPECT_EQ(msg.value("context_token", ""), "ctx-B");
    EXPECT_EQ(msg.value("message_type", 0), 2);
    EXPECT_EQ(msg["item_list"][0]["text_item"].value("text", ""), u8"粗体 文字");
    EXPECT_EQ(msg.value("client_id", "").rfind("acecode-weixin-", 0), 0u);
    EXPECT_EQ(sends[0].body["base_info"].value("channel_version", ""), "2.2.0");

    std::string longer;
    for (int i = 0; i < 4500; ++i) longer += u8"字";
    EXPECT_EQ(transport.send_text(user_address(), longer, {}).outcome, SendOutcome::Sent);
    sends = server.calls_to("sendmessage");
    ASSERT_EQ(sends.size(), 4u);
    std::string joined;
    std::set<std::string> ids;
    for (std::size_t i = 1; i < sends.size(); ++i) {
        const auto text = sends[i].body["msg"]["item_list"][0]["text_item"].value("text", "");
        EXPECT_LE(text_units(text, false), 2000u);
        joined += text;
        ids.insert(sends[i].body["msg"].value("client_id", ""));
    }
    EXPECT_EQ(joined, longer);
    EXPECT_EQ(ids.size(), 3u);
}

// 场景:在 Desktop 里输入回复(没有触发消息),传输层只有上次持久化的 context_token,
// 而它已经过期,平台对带 token 的发送返回 -14。
// 期望:去掉 token 用同一个 client_id 重发并成功;过期 token 被删除并通知调用方(token 为空)。
TEST(WeixinTransport, RetriesWithoutStaleContextToken) {
    test::FakeWeixinServer server;
    server.override = [](const test::FakeWeixinServer::Call& call) -> nlohmann::json {
        if (call.endpoint == "sendmessage" && call.body["msg"].contains("context_token"))
            return {{"errcode", -14}, {"errmsg", "session timeout"}};
        return nullptr;
    };
    auto options = local_options(server);
    options.initial_context_tokens = {{kUser, "ctx-old"}};
    std::mutex mu;
    std::vector<std::pair<std::string, std::string>> updates;
    options.on_context_token = [&](const std::string& peer, const std::string& token) {
        std::lock_guard<std::mutex> lock(mu);
        updates.emplace_back(peer, token);
    };
    WeixinTransport transport(options);
    Recorder recorder;
    transport.start(recorder.callbacks());
    EXPECT_EQ(transport.send_text(user_address(), "hello", nlohmann::json::object()).outcome, SendOutcome::Sent);
    const auto sends = server.calls_to("sendmessage");
    ASSERT_EQ(sends.size(), 2u);
    EXPECT_EQ(sends[0].body["msg"].value("context_token", ""), "ctx-old");
    EXPECT_FALSE(sends[1].body["msg"].contains("context_token"));
    EXPECT_EQ(sends[0].body["msg"].value("client_id", ""), sends[1].body["msg"].value("client_id", ""));
    EXPECT_EQ(transport.context_tokens().count(kUser), 0u);
    std::lock_guard<std::mutex> lock(mu);
    ASSERT_EQ(updates.size(), 1u);
    EXPECT_EQ(updates[0], std::make_pair(std::string(kUser), std::string{}));
}

// 场景:平台对一次发送返回 -2 "freq limit"(限频);另一次返回参数错误。
// 期望:限频时等待后用同一个 client_id 重发并成功,不丢消息;参数错误直接失败、不重发。
TEST(WeixinTransport, RateLimitWaitsAndResends) {
    test::FakeWeixinServer server;
    std::atomic<int> sends{0};
    std::atomic<bool> reject{false};
    server.override = [&](const test::FakeWeixinServer::Call& call) -> nlohmann::json {
        if (call.endpoint != "sendmessage") return nullptr;
        if (reject) return {{"ret", -1}, {"errmsg", "param error"}};
        if (++sends == 1) return {{"ret", -2}, {"errmsg", "freq limit"}};
        return nullptr;
    };
    auto options = local_options(server);
    options.rate_limit_delay = std::chrono::milliseconds(200);
    WeixinTransport transport(options);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(transport.send_text(user_address(), "hello", {}).outcome, SendOutcome::Sent);
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(180));
    auto calls = server.calls_to("sendmessage");
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].body["msg"].value("client_id", ""), calls[1].body["msg"].value("client_id", ""));

    reject = true;
    const auto failed = transport.send_text(user_address(), "again", {});
    EXPECT_EQ(failed.outcome, SendOutcome::Failed);
    EXPECT_NE(failed.error.find(u8"错误码 -1"), std::string::npos) << failed.error;
    EXPECT_EQ(server.calls_to("sendmessage").size(), 3u);
}

// 场景:会话开始忙碌时打开“正在输入”,结束时关闭。
// 期望:先用 getconfig 取 typing_ticket(带上该用户的 context_token),忙碌期间反复发送 status 1;
// 关闭时发送 status 2(否则对方一直显示“正在输入”),之后不再发送 status 1。
TEST(WeixinTransport, TypingUsesTicketAndAlwaysCancels) {
    test::FakeWeixinServer server;
    auto options = local_options(server);
    options.initial_context_tokens = {{kUser, "ctx-T"}};
    WeixinTransport transport(options);
    Recorder recorder;
    transport.start(recorder.callbacks());
    const auto typing_with = [&](int status) {
        std::size_t n = 0;
        for (const auto& call : server.calls_to("sendtyping"))
            if (call.body.value("status", 0) == status) ++n;
        return n;
    };
    transport.set_typing(user_address(), true);
    ASSERT_TRUE(test::FakeWeixinServer::wait_until([&] { return typing_with(1) >= 2; }));
    const auto config = server.calls_to("getconfig");
    ASSERT_EQ(config.size(), 1u);
    EXPECT_EQ(config[0].body.value("ilink_user_id", ""), kUser);
    EXPECT_EQ(config[0].body.value("context_token", ""), "ctx-T");
    EXPECT_EQ(server.calls_to("sendtyping").at(0).body.value("typing_ticket", ""), "ticket-1");

    transport.set_typing(user_address(), false);
    ASSERT_TRUE(test::FakeWeixinServer::wait_until([&] { return typing_with(2) == 1; }));
    const auto after = typing_with(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(typing_with(1), after);
    EXPECT_EQ(transport.capabilities().supports_typing, true);
}

// 场景:回传一份中文文件名的 PDF 与一张 PNG 图片。
// 期望:getuploadurl 带上类型(文件 3 / 图片 1)、明文大小与 MD5、填充后大小、十六进制密钥与 filekey;
// 密文 POST 到 CDN(地址由 upload_param 与 filekey 拼成),用该密钥能解回原文;
// sendmessage 的条目用 CDN 响应头 x-encrypted-param,aes_key = base64(十六进制密钥),
// 文件条目保留中文文件名、len 为十进制字符串,图片条目 mid_size = 密文大小。
TEST(WeixinTransport, UploadsFilesThroughCdn) {
    test::FakeWeixinServer server;
    WeixinTransport transport(local_options(server));
    const auto dir = temp_dir("acecode-weixin-upload");
    const std::string pdf = u8"PDF-内容-0123456789";
    { std::ofstream(dir / "r.pdf", std::ios::binary) << pdf; }
    { std::ofstream(dir / "p.png", std::ios::binary) << std::string(40, 'P'); }

    ASSERT_EQ(transport.send_file(user_address(), dir / "r.pdf", u8"报告.pdf", "application/pdf", {}).outcome,
              SendOutcome::Sent);
    const auto request = server.calls_to("getuploadurl").at(0).body;
    EXPECT_EQ(request.value("media_type", 0), 3);
    EXPECT_EQ(request.value("to_user_id", ""), kUser);
    EXPECT_EQ(request.value("rawsize", 0u), pdf.size());
    EXPECT_EQ(request.value("rawfilemd5", ""), platform::to_hex(platform::md5_digest(pdf)));
    EXPECT_EQ(request.value("filesize", 0u), platform::aes_128_ecb_padded_size(pdf.size()));
    EXPECT_TRUE(request.value("no_need_thumb", false));
    const auto key_hex = request.value("aeskey", "");
    ASSERT_EQ(key_hex.size(), 32u);
    EXPECT_EQ(request.value("filekey", "").size(), 32u);

    const auto upload = server.calls_to("cdn_upload").at(0);
    EXPECT_EQ(upload.method, "POST");
    EXPECT_EQ(upload.query.at("encrypted_query_param"), "up-1");
    EXPECT_EQ(upload.query.at("filekey"), request.value("filekey", ""));
    std::string plaintext, error;
    ASSERT_TRUE(decrypt_media(*hex_decode(key_hex), upload.raw, plaintext, &error)) << error;
    EXPECT_EQ(plaintext, pdf);

    const auto item = server.calls_to("sendmessage").at(0).body["msg"]["item_list"][0];
    EXPECT_EQ(item.value("type", 0), 4);
    EXPECT_EQ(item["file_item"].value("file_name", ""), u8"报告.pdf");
    EXPECT_EQ(item["file_item"].value("len", ""), std::to_string(pdf.size()));
    EXPECT_EQ(item["file_item"]["media"].value("encrypt_query_param", ""), "enc-1");
    EXPECT_EQ(item["file_item"]["media"].value("aes_key", ""), base64_encode(key_hex));

    ASSERT_EQ(transport.send_file(user_address(), dir / "p.png", "p.png", "image/png", {}).outcome, SendOutcome::Sent);
    EXPECT_EQ(server.calls_to("getuploadurl").at(1).body.value("media_type", 0), 1);
    const auto image = server.calls_to("sendmessage").at(1).body["msg"]["item_list"][0];
    EXPECT_EQ(image.value("type", 0), 2);
    EXPECT_EQ(image["image_item"].value("mid_size", 0u), platform::aes_128_ecb_padded_size(40));
    std::filesystem::remove_all(dir);
}

// 场景:CDN 上传先返回一次 500(临时故障);另一次返回 400(请求被拒);以及文件超过上传上限。
// 期望:500 后重新上传并成功;400 不重试直接失败;超限文件在上传前就被拒绝,原因提示上限。
TEST(WeixinTransport, CdnUploadRetriesOnlyTransientFailures) {
    test::FakeWeixinServer server;
    auto options = local_options(server);
    options.max_upload_bytes = 1024;
    WeixinTransport transport(options);
    const auto dir = temp_dir("acecode-weixin-upload-retry");
    { std::ofstream(dir / "a.txt", std::ios::binary) << "hello"; }
    { std::ofstream(dir / "big.bin", std::ios::binary) << std::string(2048, 'b'); }

    server.cdn_upload_failures = {500};
    EXPECT_EQ(transport.send_file(user_address(), dir / "a.txt", "a.txt", "text/plain", {}).outcome, SendOutcome::Sent);
    EXPECT_EQ(server.calls_to("cdn_upload").size(), 2u);

    server.cdn_upload_failures = {400};
    const auto rejected = transport.send_file(user_address(), dir / "a.txt", "a.txt", "text/plain", {});
    EXPECT_EQ(rejected.outcome, SendOutcome::Failed);
    EXPECT_EQ(server.calls_to("cdn_upload").size(), 3u);
    EXPECT_EQ(server.calls_to("sendmessage").size(), 1u);

    const auto big = transport.send_file(user_address(), dir / "big.bin", "big.bin", "", {});
    EXPECT_EQ(big.outcome, SendOutcome::Failed);
    EXPECT_NE(big.error.find(u8"上限"), std::string::npos) << big.error;
    std::filesystem::remove_all(dir);
}

// 场景:用户发来一张图片(密钥在 image_item.aeskey,内容在 CDN 上加密);另有一个附件只给了
// 指向陌生主机的 full_url,一个指向本 CDN 的 full_url,以及一个解密后超过下载上限的文件。
// 期望:图片下载并解密成原文;陌生主机被拒绝(防 SSRF);本 CDN 的 full_url 可下载;超限文件失败并删除。
TEST(WeixinTransport, DownloadsAndDecryptsMedia) {
    test::FakeWeixinServer server;
    std::string key;
    for (int i = 0; i < 16; ++i) key.push_back(static_cast<char>(0xA0 + i));
    const std::string content = u8"微信图片的原始字节 PNG...";
    std::string encrypted;
    ASSERT_TRUE(platform::aes_128_ecb_encrypt(key, content, encrypted, nullptr));
    server.put_blob("Q-IMG", encrypted);
    std::string big_encrypted;
    ASSERT_TRUE(platform::aes_128_ecb_encrypt(key, std::string(4096, 'z'), big_encrypted, nullptr));
    server.put_blob("Q-BIG", big_encrypted);

    auto message = test::FakeWeixinServer::text_message(kUser, "", 50);
    message["item_list"] = nlohmann::json::array(
        {{{"type", 2}, {"image_item", {{"aeskey", platform::to_hex(key)}, {"media", {{"encrypt_query_param", "Q-IMG"}}}}}}});
    server.push_message(message);
    auto options = local_options(server);
    options.max_download_bytes = 1024;
    WeixinTransport transport(options);
    Recorder recorder;
    transport.start(recorder.callbacks());
    ASSERT_TRUE(test::FakeWeixinServer::wait_until([&] { return recorder.count() == 1; }));
    const auto inbound = recorder.at(0);
    ASSERT_EQ(inbound.attachments.size(), 1u);
    EXPECT_EQ(inbound.attachments[0].kind, AttachmentKind::Image);

    const auto dir = temp_dir("acecode-weixin-download");
    std::string error;
    ASSERT_TRUE(transport.download(inbound.attachments[0], dir / "img.jpg", &error)) << error;
    EXPECT_EQ(read_file(dir / "img.jpg"), content);

    Attachment foreign;
    foreign.kind = AttachmentKind::File;
    foreign.remote_ref = encode_media_ref(MediaRef{"", "http://evil.example/x", key, false, "file"});
    EXPECT_FALSE(transport.download(foreign, dir / "evil.bin", &error));
    EXPECT_NE(error.find(u8"域名"), std::string::npos) << error;

    Attachment direct;
    direct.kind = AttachmentKind::File;
    direct.remote_ref = encode_media_ref(
        MediaRef{"", server.cdn_base() + "/download?encrypted_query_param=Q-IMG", key, false, "file"});
    ASSERT_TRUE(transport.download(direct, dir / "direct.bin", &error)) << error;
    EXPECT_EQ(read_file(dir / "direct.bin"), content);

    Attachment big;
    big.kind = AttachmentKind::File;
    big.remote_ref = encode_media_ref(MediaRef{"Q-BIG", "", key, false, "file"});
    EXPECT_FALSE(transport.download(big, dir / "big.bin", &error));
    EXPECT_NE(error.find(u8"上限"), std::string::npos) << error;
    EXPECT_FALSE(std::filesystem::exists(dir / "big.bin"));
    std::filesystem::remove_all(dir);
}

// 场景:长轮询进行中(服务端挂起请求)时关闭通道。
// 期望:stop 在 3 秒内返回,不必等长轮询超时;之后状态为已停止。
TEST(WeixinTransport, StopsPromptlyDuringLongPoll) {
    test::FakeWeixinServer server;
    server.max_hold = std::chrono::seconds(5);
    auto options = local_options(server);
    options.poll_timeout = std::chrono::seconds(5);
    WeixinTransport transport(options);
    Recorder recorder;
    transport.start(recorder.callbacks());
    ASSERT_TRUE(test::FakeWeixinServer::wait_until([&] { return !server.calls_to("getupdates").empty(); }));
    const auto start = std::chrono::steady_clock::now();
    transport.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
    EXPECT_EQ(transport.status().state, LinkState::Stopped);
}

// 场景:开启通道后一直没有新消息,第一次长轮询要挂满(真实平台约 35 秒)才返回。
// 期望:长轮询挂起超过 connect_grace 且没有报错(失效 token 会立即得到 -14)时就显示已连接。
// 回归:Telegram 曾因等第一次长轮询返回才置已连接,设置页打开开关后长时间停在“连接中”。
TEST(WeixinTransport, ReportsConnectedWhileFirstPollIsHeld) {
    test::FakeWeixinServer server;
    server.max_hold = std::chrono::seconds(4);
    auto options = local_options(server);
    options.poll_timeout = std::chrono::seconds(4);
    WeixinTransport transport(options);
    Recorder recorder;
    transport.start(recorder.callbacks());
    EXPECT_TRUE(test::FakeWeixinServer::wait_until(
        [&] { return transport.status().state == LinkState::Connected; }, std::chrono::milliseconds(1500)));
    EXPECT_EQ(server.calls_to("getupdates").size(), 1u);
    transport.stop();
}

} // namespace
} // namespace acecode::im::weixin
