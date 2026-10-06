#include <gtest/gtest.h>

#include "im/telegram/tg_transport.hpp"
#include "test_support/im/fake_telegram_server.hpp"

#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

// im/telegram/tg_transport:Telegram 传输层端到端测试(本机假 Bot API)。
// 覆盖:身份与隐私模式、长轮询收消息与 offset 持久化、HTML 发送与纯文本回退、
// 429 重试、webhook 冲突与用户确认移除、被占用、token 无效、正在输入、文件收发、及时停机。

namespace acecode::im::telegram {
namespace {

TelegramTransportOptions local_options(const test::FakeTelegramServer& server) {
    TelegramTransportOptions options;
    options.api.token = server.token();
    options.api.api_base = server.base();
    options.api.use_proxy = false;
    options.poll_timeout = std::chrono::seconds(1);
    options.backoff = {std::chrono::milliseconds(50)};
    options.conflict_retry = std::chrono::milliseconds(100);
    options.typing_interval = std::chrono::milliseconds(100);
    options.limits.per_chat_gap = std::chrono::milliseconds(10);
    return options;
}

struct Recorder {
    std::mutex mu;
    std::vector<Inbound> inbound;
    TransportCallbacks callbacks() {
        TransportCallbacks cb;
        cb.on_inbound = [this](Inbound in) {
            std::lock_guard<std::mutex> lock(mu);
            inbound.push_back(std::move(in));
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
};

bool wait_state(TelegramTransport& transport, LinkState state) {
    return test::FakeTelegramServer::wait_until([&] { return transport.status().state == state; });
}

// 场景:开启 Telegram,机器人隐私模式开着;随后用户私聊一条消息。
// 期望:状态变为已连接,显示 @用户名、聊天链接与“隐私模式开启”;收到入站消息;
// 处理后的 offset 通过回调交给调用方持久化(= update_id + 1)。
TEST(TelegramTransport, ConnectsReceivesAndPersistsOffset) {
    test::FakeTelegramServer server;
    auto options = local_options(server);
    std::atomic<std::int64_t> persisted{0};
    options.on_offset = [&](std::int64_t value) { persisted = value; };
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    TelegramTransport transport(options);
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    const auto status = transport.status();
    EXPECT_EQ(status.display_name, "@AceTestBot");
    EXPECT_EQ(status.extra.value("link", ""), "https://t.me/AceTestBot");
    EXPECT_TRUE(status.extra.value("privacy_mode", false));
    EXPECT_EQ(status.account, "8001");

    server.push_private_text(42, "你好", 1);
    ASSERT_TRUE(test::FakeTelegramServer::wait_until([&] { return recorder.count() == 1; }));
    EXPECT_EQ(recorder.at(0).text, u8"你好");
    EXPECT_EQ(recorder.at(0).address.account, "8001");
    ASSERT_TRUE(test::FakeTelegramServer::wait_until([&] { return persisted.load() == 101; }));
    transport.stop();
}

// 场景:重启时把持久化的 offset 交给传输层,服务端还留着旧消息。
// 期望:从该 offset 开始轮询,旧消息不会再次交给上层。
TEST(TelegramTransport, ResumesFromPersistedOffset) {
    test::FakeTelegramServer server;
    server.push_private_text(42, "old", 1);  // update_id 100
    server.push_private_text(42, "new", 2);  // update_id 101
    auto options = local_options(server);
    options.initial_offset = 101;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    TelegramTransport transport(options);
    transport.start(recorder.callbacks());
    ASSERT_TRUE(test::FakeTelegramServer::wait_until([&] { return recorder.count() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(recorder.count(), 1u);
    EXPECT_EQ(recorder.at(0).text, "new");
}

// 场景:回复一段 Markdown;第二次发送时平台说 HTML 实体解析失败。
// 期望:第一次以 HTML 发出并引用原消息;第二次同一内容改以纯文本重发成功。
TEST(TelegramTransport, SendsHtmlAndFallsBackToPlainText) {
    test::FakeTelegramServer server;
    std::atomic<int> sends{0};
    server.override = [&](const test::FakeTelegramServer::Call& call) -> nlohmann::json {
        if (call.method != "sendMessage") return nullptr;
        if (++sends == 2)
            return {{"ok", false}, {"error_code", 400}, {"description", "Bad Request: can't parse entities"}};
        return nullptr;
    };
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    TelegramTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    Address to;
    to.platform = "telegram";
    to.account = "8001";
    to.chat = to.sender = "42";
    EXPECT_EQ(transport.send_text(to, "**粗体** <tag>", {{"message_id", 55}}).outcome, SendOutcome::Sent);
    EXPECT_EQ(transport.send_text(to, "**第二条**", {{"message_id", 56}}).outcome, SendOutcome::Sent);
    const auto calls = server.calls_to("sendMessage");
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_EQ(calls[0].body.value("parse_mode", ""), "HTML");
    EXPECT_EQ(calls[0].body.value("text", ""), u8"<b>粗体</b> &lt;tag&gt;");
    EXPECT_EQ(calls[0].body["reply_parameters"].value("message_id", 0), 55);
    EXPECT_FALSE(calls[2].body.contains("parse_mode"));
    EXPECT_EQ(calls[2].body.value("text", ""), u8"第二条");
}

// 场景:平台对一次发送返回 429,要求等 1 秒。
// 期望:传输层等待后重发同一条消息并成功。
TEST(TelegramTransport, RetriesAfterRateLimit) {
    test::FakeTelegramServer server;
    std::atomic<int> sends{0};
    server.override = [&](const test::FakeTelegramServer::Call& call) -> nlohmann::json {
        if (call.method != "sendMessage" || ++sends > 1) return nullptr;
        return {{"ok", false}, {"error_code", 429}, {"description", "Too Many Requests"}, {"parameters", {{"retry_after", 1}}}};
    };
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    TelegramTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    Address to;
    to.platform = "telegram";
    to.account = "8001";
    to.chat = to.sender = "42";
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(transport.send_text(to, "hello", {}).outcome, SendOutcome::Sent);
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(900));
    EXPECT_EQ(server.calls_to("sendMessage").size(), 2u);
}

// 场景:机器人配置了 webhook,getUpdates 返回 409;用户在设置页确认“移除 webhook 并连接”。
// 期望:先进入失败状态并标出 webhook;执行移除动作后调用 deleteWebhook,随后恢复轮询并连上。
TEST(TelegramTransport, WebhookConflictWaitsForUserConfirmation) {
    test::FakeTelegramServer server;
    std::atomic<bool> webhook{true};
    server.override = [&](const test::FakeTelegramServer::Call& call) -> nlohmann::json {
        if (call.method == "deleteWebhook") {
            webhook = false;
            return nullptr;
        }
        if (call.method == "getUpdates" && webhook)
            return {{"ok", false}, {"error_code", 409},
                    {"description", "Conflict: can't use getUpdates method while webhook is active"}};
        return nullptr;
    };
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    TelegramTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Failed));
    EXPECT_TRUE(transport.status().extra.value("webhook", false));
    EXPECT_TRUE(server.calls_to("deleteWebhook").empty());
    EXPECT_TRUE(transport.action("remove_webhook", nlohmann::json::object()).value("ok", false));
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    EXPECT_EQ(server.calls_to("deleteWebhook").size(), 1u);
}

// 场景:另一个程序正在用同一个 token 长轮询;以及 token 已被吊销。
// 期望:前者显示“被其他程序占用”并稍后重试;后者直接失败、停止重试。
TEST(TelegramTransport, ReportsOtherPollerAndInvalidToken) {
    test::FakeTelegramServer server;
    server.override = [&](const test::FakeTelegramServer::Call& call) -> nlohmann::json {
        if (call.method != "getUpdates") return nullptr;
        return {{"ok", false}, {"error_code", 409}, {"description", "Conflict: terminated by other getUpdates request"}};
    };
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    TelegramTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Retrying));
    EXPECT_NE(transport.status().detail.find(u8"占用"), std::string::npos);
    transport.stop();

    auto options = local_options(server);
    options.api.token = "999999:WRONG-TOKEN-zyxwvutsrqponmlkjihgfedcba";
    TelegramTransport bad(options);
    bad.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(bad, LinkState::Failed));
    EXPECT_TRUE(bad.status().retry_stopped);
    EXPECT_EQ(bad.status().detail.find("WRONG-TOKEN"), std::string::npos);
}

// 场景:会话开始忙碌时打开“正在输入”,结束时关闭。
// 期望:忙碌期间持续发送 sendChatAction(typing);关闭后不再发送。
TEST(TelegramTransport, TypingIndicatorRepeatsWhileBusy) {
    test::FakeTelegramServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    TelegramTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    Address to;
    to.platform = "telegram";
    to.account = "8001";
    to.chat = to.sender = "42";
    transport.set_typing(to, true);
    ASSERT_TRUE(test::FakeTelegramServer::wait_until([&] { return server.calls_to("sendChatAction").size() >= 2; }));
    transport.set_typing(to, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto after = server.calls_to("sendChatAction").size();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(server.calls_to("sendChatAction").size(), after);
    EXPECT_EQ(server.calls_to("sendChatAction").at(0).body.value("action", ""), "typing");
}

// 场景:回传一个中文文件名的文档和一张图片;再下载用户发来的文件,以及一个超大文件。
// 期望:文档走 sendDocument,multipart 文件名降为 ASCII(全是非 ASCII 时用 file)、原名放进 caption;
// 图片走 sendPhoto;
// 下载经 getFile 拿到路径后成功;超出 20 MB 的文件被拒。
TEST(TelegramTransport, SendsAndDownloadsFiles) {
    test::FakeTelegramServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    TelegramTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    Address to;
    to.platform = "telegram";
    to.account = "8001";
    to.chat = to.sender = "42";
    const auto dir = std::filesystem::path(testing::TempDir()) / "acecode-tg-files";
    std::filesystem::create_directories(dir);
    { std::ofstream(dir / "r.pdf", std::ios::binary) << "PDF"; }
    { std::ofstream(dir / "p.png", std::ios::binary) << "PNG"; }
    EXPECT_EQ(transport.send_file(to, dir / "r.pdf", u8"报告.pdf", "application/pdf", {}).outcome, SendOutcome::Sent);
    EXPECT_EQ(transport.send_file(to, dir / "p.png", "p.png", "image/png", {}).outcome, SendOutcome::Sent);
    const auto docs = server.calls_to("sendDocument");
    ASSERT_EQ(docs.size(), 1u);
    EXPECT_EQ(docs[0].file_name, "file.pdf");
    EXPECT_EQ(docs[0].body.value("caption", ""), u8"报告.pdf");
    EXPECT_EQ(docs[0].file_body, "PDF");
    EXPECT_EQ(server.calls_to("sendPhoto").size(), 1u);

    Attachment attachment;
    attachment.kind = AttachmentKind::File;
    attachment.remote_ref = "f1";
    std::string error;
    ASSERT_TRUE(transport.download(attachment, dir / "in.bin", &error)) << error;
    EXPECT_EQ(std::filesystem::file_size(dir / "in.bin"), 32u);
    attachment.remote_ref = "too-big";
    EXPECT_FALSE(transport.download(attachment, dir / "big.bin", &error));
    std::filesystem::remove_all(dir);
}

// 场景:长轮询进行中(服务端挂起请求)时关闭通道。
// 期望:stop 在 3 秒内返回,不必等长轮询超时。
TEST(TelegramTransport, StopsPromptlyDuringLongPoll) {
    test::FakeTelegramServer server;
    auto options = local_options(server);
    options.poll_timeout = std::chrono::seconds(25);
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    TelegramTransport transport(options);
    transport.start(recorder.callbacks());
    ASSERT_TRUE(test::FakeTelegramServer::wait_until([&] { return !server.calls_to("getUpdates").empty(); }));
    const auto start = std::chrono::steady_clock::now();
    transport.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
    EXPECT_EQ(transport.status().state, LinkState::Stopped);
}

// 场景:打开开关后一直没有新消息,第一次长轮询要挂到超时才返回(真实平台是 25 秒)。
// 期望:getMe 成功后立即显示已连接,不必等第一次长轮询返回。
// 回归:曾经要等第一次 getUpdates 返回才置已连接,设置页打开开关后长时间停在“连接中”。
TEST(TelegramTransport, ReportsConnectedBeforeFirstLongPollReturns) {
    test::FakeTelegramServer server;
    std::atomic<bool> release{false};
    server.override = [&](const test::FakeTelegramServer::Call& call) -> nlohmann::json {
        if (call.method != "getUpdates") return nullptr;
        for (int i = 0; i < 150 && !release; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return {{"ok", true}, {"result", nlohmann::json::array()}};
    };
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    TelegramTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    EXPECT_TRUE(test::FakeTelegramServer::wait_until(
        [&] { return transport.status().state == LinkState::Connected; }, std::chrono::milliseconds(1000)));
    release = true;
    transport.stop();
}

} // namespace
} // namespace acecode::im::telegram
