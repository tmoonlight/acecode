#include <gtest/gtest.h>

#include "channels/core/line_webhook_server.hpp"
#include "im/line/line_transport.hpp"
#include "test_support/im/fake_line_server.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

// im/line/line_transport:LINE 传输层端到端测试。
// 真实组件:LineTransport + Crow 回调端口(channels::core::LineWebhookServer);
// 假组件:本机假 LINE API、假 cloudflared。所有请求只在 127.0.0.1,不连真实平台。
// 覆盖:自有公网地址 / 快速隧道两种模式的连接与 webhook 登记、签名校验、去重、私聊与群 @、
// 回复与推送的选择、5 条一批、推送重试键、额度用完暂存补发、图片临时链接、文件说明、
// 凭据无效停止重试、未安装 cloudflared、隧道退出重建、Use webhook 未开、正在输入、引用识别、及时停机。

namespace acecode::im::line {
namespace {

using test::FakeCloudflared;
using test::FakeLineServer;

constexpr const char* kUser = "U11111111111111111111111111111111";
constexpr const char* kGroup = "Cgroup0000000000000000000000000001";

struct Recorder {
    std::mutex mu;
    std::vector<Inbound> inbound;
    std::function<void()> on_each;  // 可选:收到入站消息时(在传输层线程上)调用

    TransportCallbacks callbacks() {
        TransportCallbacks cb;
        cb.on_inbound = [this](Inbound in) {
            if (on_each) on_each();
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

struct Harness {
    FakeLineServer server;
    std::shared_ptr<channels::core::LineWebhookServer> listener = std::make_shared<channels::core::LineWebhookServer>();
    std::uint16_t port = pick_free_loopback_port();

    std::string base() const { return "http://127.0.0.1:" + std::to_string(port); }
    std::string webhook_url() const { return base() + "/line/webhook"; }

    // 自有公网地址模式:公网地址就是回调端口本身,公网自检与 LINE 的 webhook 测试都直达它。
    LineTransportOptions options() const {
        LineTransportOptions options;
        options.api.channel_id = FakeLineServer::kChannelId;
        options.api.channel_secret = FakeLineServer::kSecret;
        options.api.api_base = server.base();
        options.api.data_api_base = server.base();
        options.api.use_proxy = false;
        options.api.timeout = std::chrono::seconds(5);
        options.listener = listener;
        options.listen_port = port;
        options.public_url = base() + "/";
        options.backoff = {std::chrono::milliseconds(50)};
        options.public_check_timeout = std::chrono::seconds(3);
        options.public_check_interval = std::chrono::milliseconds(50);
        options.watch_interval = std::chrono::hours(1);
        options.webhook_recheck = std::chrono::milliseconds(100);
        options.send_retry = {std::chrono::milliseconds(20), std::chrono::milliseconds(20)};
        return options;
    }
};

Address private_address() {
    Address address;
    address.platform = "line";
    address.account = FakeLineServer::kBotId;
    address.kind = ChatKind::Private;
    address.chat = address.sender = kUser;
    return address;
}

Address group_address() {
    auto address = private_address();
    address.kind = ChatKind::Group;
    address.chat = kGroup;
    return address;
}

bool wait_state(LineTransport& transport, LinkState state) {
    return FakeLineServer::wait_until([&transport, state] { return transport.status().state == state; });
}

// 发一条私聊文字 webhook,返回回调端口的 HTTP 状态码。
long post_private_text(Harness& h, const std::string& text, const std::string& message_id,
                       const std::string& reply_token, const std::string& event_id) {
    const auto body = FakeLineServer::webhook_body(
        {FakeLineServer::text_event(FakeLineServer::source("user", "", kUser), text, message_id, reply_token, event_id)});
    return FakeLineServer::post_webhook(h.webhook_url(), body).status;
}

// 场景:使用自己的公网地址(结尾带斜杠)开启 LINE。
// 期望:换令牌 → 读机器人信息 → 开回调端口 → 公网自检 → PUT 登记 <公网地址>/line/webhook →
// 调用 LINE 的 webhook 测试(假服务真的发来带签名的请求)→ 已连接;状态里有机器人 id、名称、加好友链接。
TEST(LineTransport, ConnectsWithOwnPublicUrlAndRegistersWebhook) {
    Harness h;
    Recorder recorder;  // 先于传输层声明:传输层析构(停机)时回调对象仍然有效
    LineTransport transport(h.options());
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected)) << transport.status().detail;
    const auto status = transport.status();
    EXPECT_EQ(status.account, FakeLineServer::kBotId);
    EXPECT_EQ(status.display_name, u8"ACE 测试机器人");
    EXPECT_EQ(status.extra.value("add_friend_url", ""), "https://line.me/R/ti/p/%40216ruabc");
    EXPECT_EQ(status.extra.value("webhook_url", ""), h.webhook_url());
    EXPECT_EQ(status.extra.value("listen_port", 0), h.port);
    EXPECT_EQ(status.extra.value("tunnel", ""), "custom");
    EXPECT_FALSE(status.extra.value("setup_hint", "").empty());
    EXPECT_EQ(h.server.endpoint(), h.webhook_url());
    EXPECT_EQ(h.server.calls_to("PUT", "/v2/bot/channel/webhook/endpoint").size(), 1u);
    EXPECT_EQ(h.server.calls_to("POST", "/v2/bot/channel/webhook/test").size(), 1u);
    EXPECT_EQ(h.server.mint_count(), 1);
    EXPECT_EQ(recorder.count(), 0u);  // webhook 测试是空事件,不产生入站消息
    const auto begin = std::chrono::steady_clock::now();
    transport.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(3));
    EXPECT_EQ(transport.status().state, LinkState::Stopped);
}

// 场景:用户私聊发来一条文字;LINE 因网络原因重发同一事件;另有伪造签名与无签名的请求。
// 期望:回调立即 200,入站消息只上报一次(按 webhookEventId 去重),带发言人显示名与回复令牌;
// 签名不对或缺失一律 401 且不上报。
TEST(LineTransport, ReceivesPrivateTextOnceAndRejectsBadSignatures) {
    Harness h;
    Recorder recorder;  // 先于传输层声明:传输层析构(停机)时回调对象仍然有效
    LineTransport transport(h.options());
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected)) << transport.status().detail;
    const auto token = h.server.issue_reply_token();
    EXPECT_EQ(post_private_text(h, u8"你好", "m1", token, "ev1"), 200);
    ASSERT_TRUE(FakeLineServer::wait_until([&recorder] { return recorder.count() == 1; }));
    const auto in = recorder.at(0);
    EXPECT_EQ(in.address.platform, "line");
    EXPECT_EQ(in.address.account, FakeLineServer::kBotId);
    EXPECT_EQ(in.address.chat, kUser);
    EXPECT_EQ(in.address.sender, kUser);
    EXPECT_EQ(in.text, u8"你好");
    EXPECT_TRUE(in.mentioned);
    EXPECT_EQ(in.sender_name, "Ann");
    EXPECT_EQ(in.reply_context.value("reply_token", ""), token);

    EXPECT_EQ(post_private_text(h, u8"你好", "m1", token, "ev1"), 200);  // 重投
    const auto body = FakeLineServer::webhook_body(
        {FakeLineServer::text_event(FakeLineServer::source("user", "", kUser), "evil", "m2", "", "ev2")});
    EXPECT_EQ(FakeLineServer::post_webhook(h.webhook_url(), body, "ffffffffffffffffffffffffffffffff").status, 401);
    EXPECT_EQ(FakeLineServer::post_webhook(h.webhook_url(), body, "").status, 401);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(recorder.count(), 1u);
}

// 场景:群里一条 @机器人 的消息,一条普通闲聊。
// 期望:两条都上报(是否回应由核心决定);前者 mentioned=true 且去掉了 @ 部分,后者 mentioned=false;
// 群地址 chat = groupId、sender = 发言人。
TEST(LineTransport, GroupMentionsAreDetected) {
    Harness h;
    Recorder recorder;  // 先于传输层声明:传输层析构(停机)时回调对象仍然有效
    LineTransport transport(h.options());
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected)) << transport.status().detail;
    const auto src = FakeLineServer::source("group", kGroup, kUser);
    const nlohmann::json mention{{"mention", {{"mentionees", {{{"index", 0}, {"length", 4}, {"type", "user"},
                                                               {"userId", FakeLineServer::kBotId}, {"isSelf", true}}}}}}};
    const auto body = FakeLineServer::webhook_body(
        {FakeLineServer::text_event(src, u8"@ACE 总结一下", "g1", h.server.issue_reply_token(), "eg1", mention),
         FakeLineServer::text_event(src, u8"今天天气不错", "g2", h.server.issue_reply_token(), "eg2")});
    EXPECT_EQ(FakeLineServer::post_webhook(h.webhook_url(), body).status, 200);
    ASSERT_TRUE(FakeLineServer::wait_until([&recorder] { return recorder.count() == 2; }));
    EXPECT_EQ(recorder.at(0).address.kind, ChatKind::Group);
    EXPECT_EQ(recorder.at(0).address.chat, kGroup);
    EXPECT_EQ(recorder.at(0).address.sender, kUser);
    EXPECT_TRUE(recorder.at(0).mentioned);
    EXPECT_EQ(recorder.at(0).text, u8"总结一下");
    EXPECT_FALSE(recorder.at(1).mentioned);
}

// 场景:对一条新消息先回复一段 Markdown,再用同一上下文发第二段;之后对另一条消息发 7 段长回答。
// 期望:第一段用回复令牌(免费,纯文本);令牌只能用一次,第二段改用推送并带重试键;
// 7 段长回答:前 5 段一次回复,剩下 2 段一次推送。
TEST(LineTransport, RepliesOnceThenPushesInBatchesOfFive) {
    Harness h;
    Recorder recorder;  // 先于传输层声明:传输层析构(停机)时回调对象仍然有效
    LineTransport transport(h.options());
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected)) << transport.status().detail;
    ASSERT_EQ(post_private_text(h, "q1", "m1", h.server.issue_reply_token(), "ev1"), 200);
    ASSERT_TRUE(FakeLineServer::wait_until([&recorder] { return recorder.count() == 1; }));
    const auto context = recorder.at(0).reply_context;

    EXPECT_EQ(transport.send_text(private_address(), "**你好**", context).outcome, SendOutcome::Sent);
    EXPECT_EQ(transport.send_text(private_address(), "second", context).outcome, SendOutcome::Sent);
    const auto replies = h.server.calls_to("POST", "/v2/bot/message/reply");
    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(replies[0].body["messages"][0]["text"], u8"你好");
    auto pushes = h.server.calls_to("POST", "/v2/bot/message/push");
    ASSERT_EQ(pushes.size(), 1u);
    EXPECT_EQ(pushes[0].body.value("to", ""), kUser);
    EXPECT_EQ(pushes[0].body["messages"][0]["text"], "second");
    EXPECT_FALSE(pushes[0].retry_key.empty());

    ASSERT_EQ(post_private_text(h, "q2", "m2", h.server.issue_reply_token(), "ev2"), 200);
    ASSERT_TRUE(FakeLineServer::wait_until([&recorder] { return recorder.count() == 2; }));
    std::string long_text;
    for (int i = 0; i < 7; ++i) long_text += (i ? "\n\n" : "") + std::string(4000, static_cast<char>('a' + i));
    EXPECT_EQ(transport.send_text(private_address(), long_text, recorder.at(1).reply_context).outcome, SendOutcome::Sent);
    const auto second_reply = h.server.calls_to("POST", "/v2/bot/message/reply");
    ASSERT_EQ(second_reply.size(), 2u);
    EXPECT_EQ(second_reply[1].body["messages"].size(), 5u);
    pushes = h.server.calls_to("POST", "/v2/bot/message/push");
    ASSERT_EQ(pushes.size(), 2u);
    EXPECT_EQ(pushes[1].body["messages"].size(), 2u);
    EXPECT_EQ(pushes[1].body["messages"][1]["text"].get<std::string>()[0], 'g');
}

// 场景:回复令牌已过期(2 分钟前收到),第一次推送遇到 500。
// 期望:不尝试过期令牌,直接推送;5xx 后用同一个重试键重发(LINE 据此去重),最终成功。
TEST(LineTransport, StaleTokenPushesAndRetriesWithSameKey) {
    Harness h;
    std::atomic<int> pushes{0};
    h.server.override = [&pushes](const FakeLineServer::Call& call) -> std::optional<FakeLineServer::Reply> {
        if (call.path != "/v2/bot/message/push" || ++pushes > 1) return std::nullopt;
        return FakeLineServer::Reply{500, {{"message", "Internal error"}}, {}};
    };
    Recorder recorder;  // 先于传输层声明:传输层析构(停机)时回调对象仍然有效
    LineTransport transport(h.options());
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected)) << transport.status().detail;
    ReplyToken stale;
    stale.token = h.server.issue_reply_token();
    stale.received_at_ms = FakeLineServer::now_ms() - 120'000;
    stale.event_ts_ms = stale.received_at_ms;
    EXPECT_EQ(transport.send_text(private_address(), "late answer", make_reply_context(stale, "m9")).outcome,
              SendOutcome::Sent);
    EXPECT_TRUE(h.server.calls_to("POST", "/v2/bot/message/reply").empty());
    const auto calls = h.server.calls_to("POST", "/v2/bot/message/push");
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_FALSE(calls[0].retry_key.empty());
    EXPECT_EQ(calls[0].retry_key, calls[1].retry_key);
}

// 场景:推送额度用完(429 “monthly limit”),期间有两段输出;随后对方又发来一条消息。
// 期望:两段都暂存(第二段排在第一段后面),状态标出额度用完;新消息到来时先用它的回复令牌
// 一次补发两段(第一段带“(补发)”),然后才把新消息交给核心。
TEST(LineTransport, QuotaExhaustedOutputsAreHeldAndFlushedOnNextMessage) {
    Harness h;
    h.server.override = [](const FakeLineServer::Call& call) -> std::optional<FakeLineServer::Reply> {
        if (call.path != "/v2/bot/message/push") return std::nullopt;
        return FakeLineServer::Reply{429, {{"message", "You have reached your monthly limit."}}, {}};
    };
    std::atomic<std::size_t> replies_at_inbound{0};
    Recorder recorder;  // 先于传输层声明:传输层析构(停机)时回调对象仍然有效
    LineTransport transport(h.options());
    recorder.on_each = [&replies_at_inbound, &h] {
        replies_at_inbound = h.server.calls_to("POST", "/v2/bot/message/reply").size();
    };
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected)) << transport.status().detail;

    const auto first = transport.send_text(private_address(), u8"回答一", nlohmann::json::object());
    EXPECT_EQ(first.outcome, SendOutcome::Held);
    EXPECT_FALSE(first.error.empty());
    EXPECT_EQ(transport.send_text(private_address(), u8"回答二", nlohmann::json::object()).outcome, SendOutcome::Held);
    EXPECT_EQ(transport.held_count(), 2u);
    EXPECT_TRUE(transport.status().extra.value("push_quota_exhausted", false));
    EXPECT_EQ(h.server.calls_to("POST", "/v2/bot/message/push").size(), 1u);  // 第二段直接排队,不再撞墙

    ASSERT_EQ(post_private_text(h, u8"在吗", "m5", h.server.issue_reply_token(), "ev5"), 200);
    ASSERT_TRUE(FakeLineServer::wait_until([&recorder] { return recorder.count() == 1; }));
    const auto replies = h.server.calls_to("POST", "/v2/bot/message/reply");
    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(replies_at_inbound.load(), 1u);  // 补发先于交给核心
    ASSERT_EQ(replies[0].body["messages"].size(), 2u);
    EXPECT_EQ(replies[0].body["messages"][0]["text"], u8"(补发)回答一");
    EXPECT_EQ(replies[0].body["messages"][1]["text"], u8"回答二");
    EXPECT_EQ(transport.held_count(), 0u);
}

// 场景:助手产出一张 PNG 截图和一个 PDF 报告。
// 期望:图片作为 image 消息发出,地址是 <公网地址>/line/media/<令牌>/<文件名>,经回调端口能取回原文件;
// PDF 改发一条说明(LINE 机器人不能发文件)。不存在的路径直接失败。
TEST(LineTransport, SendsImagesViaMediaRouteAndNoticesForOtherFiles) {
    Harness h;
    Recorder recorder;  // 先于传输层声明:传输层析构(停机)时回调对象仍然有效
    LineTransport transport(h.options());
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected)) << transport.status().detail;
    const auto dir = std::filesystem::temp_directory_path() / "acecode-line-transport-test";
    std::filesystem::create_directories(dir);
    const auto png = dir / "shot.png";
    const std::string bytes = "\x89PNG\r\n\x1a\nfake-image-bytes";
    {
        std::ofstream out(png, std::ios::binary);
        out << bytes;
    }
    const auto pdf = dir / "report.pdf";
    {
        std::ofstream out(pdf, std::ios::binary);
        out << "%PDF-1.4";
    }
    EXPECT_EQ(transport.send_file(private_address(), png, "shot.png", "image/png", nlohmann::json::object()).outcome,
              SendOutcome::Sent);
    auto pushes = h.server.calls_to("POST", "/v2/bot/message/push");
    ASSERT_EQ(pushes.size(), 1u);
    const auto message = pushes[0].body["messages"][0];
    EXPECT_EQ(message["type"], "image");
    const auto url = message.value("originalContentUrl", std::string{});
    EXPECT_EQ(url.rfind(h.base() + "/line/media/", 0), 0u) << url;
    EXPECT_EQ(message.value("previewImageUrl", ""), url);
    const auto fetched = FakeLineServer::http_get(url);
    EXPECT_EQ(fetched.status, 200);
    EXPECT_EQ(fetched.body, bytes);
    EXPECT_EQ(FakeLineServer::http_get(url + "x").status, 404);

    EXPECT_EQ(transport.send_file(private_address(), pdf, "report.pdf", "application/pdf", nlohmann::json::object())
                  .outcome,
              SendOutcome::Sent);
    pushes = h.server.calls_to("POST", "/v2/bot/message/push");
    ASSERT_EQ(pushes.size(), 2u);
    EXPECT_NE(pushes[1].body["messages"][0].value("text", "").find("不能发送文件"), std::string::npos);

    EXPECT_EQ(transport.send_file(private_address(), dir / "missing.png", "missing.png", "image/png", {}).outcome,
              SendOutcome::Failed);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// 场景:Channel secret 填错。
// 期望:Failed 且停止重试,原因说明 Channel ID / secret 无效;不会反复换令牌。
TEST(LineTransport, InvalidCredentialsStopRetrying) {
    Harness h;
    auto options = h.options();
    options.api.channel_secret = "ffffffffffffffffffffffffffffffff";
    Recorder recorder;  // 先于传输层声明:传输层析构(停机)时回调对象仍然有效
    LineTransport transport(options);
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Failed));
    const auto status = transport.status();
    EXPECT_TRUE(status.retry_stopped);
    EXPECT_NE(status.detail.find("Channel ID 或 Channel secret 无效"), std::string::npos);
    EXPECT_EQ(status.detail.find(options.api.channel_secret), std::string::npos);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(h.server.calls_to("POST", "/oauth2/v3/token").size(), 1u);
}

// 场景:没有填公网地址,电脑上也没装 cloudflared。
// 期望:Failed 且停止重试,提示 winget 安装命令或填写自己的公网地址。
TEST(LineTransport, MissingCloudflaredFailsWithInstallHint) {
    Harness h;
    auto options = h.options();
    options.public_url.clear();
    options.tunnel.locate = [] { return std::string{}; };
    Recorder recorder;  // 先于传输层声明:传输层析构(停机)时回调对象仍然有效
    LineTransport transport(options);
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Failed));
    EXPECT_TRUE(transport.status().retry_stopped);
    EXPECT_NE(transport.status().detail.find("winget install --id Cloudflare.cloudflared"), std::string::npos);
    EXPECT_TRUE(h.server.endpoint().empty());
}

TunnelOptions fake_tunnel(const FakeCloudflared& fake) {
    TunnelOptions tunnel;
    tunnel.locate = [] { return std::string("C:/tools/cloudflared.exe"); };
    tunnel.launch = fake.launcher();
    tunnel.metrics_port = fake.metrics_port();
    tunnel.url_scheme = "http";
    tunnel.ready_timeout = std::chrono::seconds(5);
    tunnel.poll_interval = std::chrono::milliseconds(20);
    return tunnel;
}

// 场景:不填公网地址,用(假的)Cloudflare 快速隧道;随后 cloudflared 进程意外退出。
// 期望:隧道指向回调端口(--url http://127.0.0.1:<端口>),经隧道地址自检后登记 webhook 并连上;
// 进程退出后自动重启隧道并重新连上。
TEST(LineTransport, QuickTunnelModeConnectsAndRestartsAfterExit) {
    Harness h;
    FakeCloudflared fake;
    auto options = h.options();
    options.public_url.clear();
    options.listen_port = 0;
    options.tunnel = fake_tunnel(fake);
    options.watch_interval = std::chrono::milliseconds(100);
    Recorder recorder;  // 先于传输层声明:传输层析构(停机)时回调对象仍然有效
    LineTransport transport(options);
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected)) << transport.status().detail;
    const auto port = transport.listen_port();
    ASSERT_NE(port, 0);
    const auto local = "http://127.0.0.1:" + std::to_string(port);
    EXPECT_EQ(h.server.endpoint(), local + "/line/webhook");
    EXPECT_EQ(transport.status().extra.value("tunnel", ""), "cloudflare");
    const auto argv = fake.last_argv();
    EXPECT_NE(std::find(argv.begin(), argv.end(), local), argv.end());

    fake.kill_current();
    ASSERT_TRUE(FakeLineServer::wait_until([&fake] { return fake.launch_count() == 2; }));
    ASSERT_TRUE(wait_state(transport, LinkState::Connected)) << transport.status().detail;
    transport.stop();
}

// 场景:快速隧道每次重启主机名都会变(不做公网自检的配置下)。
// 期望:重建隧道后把新地址重新登记给 LINE。
TEST(LineTransport, NewTunnelHostnameIsRegisteredAgain) {
    Harness h;
    FakeCloudflared fake;
    fake.state()->hostname_for = [](int launch, const std::string&) {
        return "tunnel-" + std::to_string(launch) + ".test";
    };
    auto options = h.options();
    options.public_url.clear();
    options.listen_port = 0;
    options.verify_public_url = false;
    options.test_webhook = false;
    options.tunnel = fake_tunnel(fake);
    options.watch_interval = std::chrono::milliseconds(100);
    Recorder recorder;  // 先于传输层声明:传输层析构(停机)时回调对象仍然有效
    LineTransport transport(options);
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected)) << transport.status().detail;
    EXPECT_EQ(h.server.endpoint(), "http://tunnel-1.test/line/webhook");
    fake.kill_current();
    ASSERT_TRUE(FakeLineServer::wait_until(
        [&h] { return h.server.endpoint() == "http://tunnel-2.test/line/webhook"; }));
    EXPECT_EQ(h.server.calls_to("PUT", "/v2/bot/channel/webhook/endpoint").size(), 2u);
}

// 场景:“Use webhook” 开关在控制台没打开;过一会儿用户打开了它。
// 期望:先处于重试中并提示去开 Use webhook(webhook 地址已登记好);开关打开后自动变为已连接。
TEST(LineTransport, WaitsForUseWebhookSwitch) {
    Harness h;
    h.server.webhook_active = false;
    Recorder recorder;  // 先于传输层声明:传输层析构(停机)时回调对象仍然有效
    LineTransport transport(h.options());
    transport.start(recorder.callbacks());
    ASSERT_TRUE(FakeLineServer::wait_until([&transport] {
        const auto status = transport.status();
        return status.state == LinkState::Retrying && status.detail.find("Use webhook") != std::string::npos;
    }));
    EXPECT_EQ(h.server.endpoint(), h.webhook_url());
    h.server.webhook_active = true;
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    EXPECT_TRUE(transport.status().extra.value("webhook_active", false));
}

// 场景:核心在私聊与群聊里都打开“正在输入”。
// 期望:私聊调用 loading/start(60 秒);群聊不调用(LINE 只允许一对一);关闭后不再续显。
TEST(LineTransport, TypingUsesLoadingAnimationForPrivateChatsOnly) {
    Harness h;
    Recorder recorder;  // 先于传输层声明:传输层析构(停机)时回调对象仍然有效
    LineTransport transport(h.options());
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected)) << transport.status().detail;
    transport.set_typing(group_address(), true);
    transport.set_typing(private_address(), true);
    ASSERT_TRUE(FakeLineServer::wait_until(
        [&h] { return !h.server.calls_to("POST", "/v2/bot/chat/loading/start").empty(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto calls = h.server.calls_to("POST", "/v2/bot/chat/loading/start");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].body.value("chatId", ""), kUser);
    EXPECT_EQ(calls[0].body.value("loadingSeconds", 0), 60);
    transport.set_typing(private_address(), false);
    transport.set_typing(group_address(), false);
}

// 场景:群里机器人回答了一条消息;之后有人引用这条回答发言(没有 @);
// 另有一位电脑版 LINE 用户(没有 userId)@了机器人。
// 期望:引用机器人的消息视同点名,并带上被引用的文字;电脑版用户的消息不上报,
// 但用免费回复令牌提示改用手机版。
TEST(LineTransport, QuotedBotMessagesAndUnidentifiedSenders) {
    Harness h;
    Recorder recorder;  // 先于传输层声明:传输层析构(停机)时回调对象仍然有效
    LineTransport transport(h.options());
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected)) << transport.status().detail;
    const auto src = FakeLineServer::source("group", kGroup, kUser);
    const nlohmann::json mention{{"mention", {{"mentionees", {{{"index", 0}, {"length", 4}, {"type", "user"},
                                                               {"isSelf", true}}}}}}};
    auto body = FakeLineServer::webhook_body(
        {FakeLineServer::text_event(src, u8"@ACE 几点了", "g1", h.server.issue_reply_token(), "eq1", mention)});
    ASSERT_EQ(FakeLineServer::post_webhook(h.webhook_url(), body).status, 200);
    ASSERT_TRUE(FakeLineServer::wait_until([&recorder] { return recorder.count() == 1; }));
    ASSERT_EQ(transport.send_text(group_address(), u8"三点", recorder.at(0).reply_context).outcome, SendOutcome::Sent);
    const auto reply = h.server.calls_to("POST", "/v2/bot/message/reply").at(0);
    EXPECT_EQ(reply.body["messages"][0].value("quoteToken", ""), "q-g1");  // 群里引用提问的那条

    const auto bot_message_id = std::string("9001");  // 假服务给第一条发出的消息分配的 id
    body = FakeLineServer::webhook_body({FakeLineServer::text_event(
        src, u8"为什么", "g2", h.server.issue_reply_token(), "eq2", {{"quotedMessageId", bot_message_id}})});
    ASSERT_EQ(FakeLineServer::post_webhook(h.webhook_url(), body).status, 200);
    ASSERT_TRUE(FakeLineServer::wait_until([&recorder] { return recorder.count() == 2; }));
    EXPECT_TRUE(recorder.at(1).mentioned);
    EXPECT_EQ(recorder.at(1).quote_text, u8"三点");

    const auto desktop_token = h.server.issue_reply_token();
    body = FakeLineServer::webhook_body({FakeLineServer::text_event(FakeLineServer::source("group", kGroup, ""),
                                                                    u8"@ACE hi", "g3", desktop_token, "eq3", mention)});
    ASSERT_EQ(FakeLineServer::post_webhook(h.webhook_url(), body).status, 200);
    ASSERT_TRUE(FakeLineServer::wait_until(
        [&h] { return h.server.calls_to("POST", "/v2/bot/message/reply").size() == 2; }));
    const auto notice = h.server.calls_to("POST", "/v2/bot/message/reply").at(1);
    EXPECT_EQ(notice.body.value("replyToken", ""), desktop_token);
    EXPECT_NE(notice.body["messages"][0].value("text", "").find("手机版"), std::string::npos);
    EXPECT_EQ(recorder.count(), 2u);
}

// 场景:收到一张图片后由核心下载;另有一个超过本地上限的文件。
// 期望:按消息 id 从内容接口下载成功;超限的在下载前就拒绝。
TEST(LineTransport, DownloadsAttachments) {
    Harness h;
    h.server.set_content("img7", "jpeg-bytes");
    auto options = h.options();
    options.max_download_bytes = 1024;
    LineTransport transport(options);
    Attachment image;
    image.kind = AttachmentKind::Image;
    image.remote_ref = "img7";
    const auto dest = std::filesystem::temp_directory_path() / "acecode-line-download.bin";
    std::string error;
    ASSERT_TRUE(transport.download(image, dest, &error)) << error;
    EXPECT_EQ(std::filesystem::file_size(dest), 10u);
    Attachment big;
    big.kind = AttachmentKind::File;
    big.remote_ref = "img7";
    big.size = 4096;
    EXPECT_FALSE(transport.download(big, dest, &error));
    EXPECT_EQ(error, "附件超过大小限制");
    std::error_code ec;
    std::filesystem::remove(dest, ec);
}

// 场景:回调端口收到超大请求体、未知路径、错误方法。
// 期望:413 / 404 / 405,都不会进入解析。
TEST(LineTransport, RouterRejectsOversizeUnknownAndWrongMethod) {
    Harness h;
    LineTransport transport(h.options());
    ListenerRequest request;
    request.method = "POST";
    request.path = kWebhookRoute;
    request.body = std::string(kMaxWebhookBodyBytes + 1, 'x');
    EXPECT_EQ(transport.handle_request(request).status, 413);
    request.path = "/api/sessions";
    EXPECT_EQ(transport.handle_request(request).status, 404);
    request.method = "GET";
    request.path = kWebhookRoute;
    EXPECT_EQ(transport.handle_request(request).status, 405);
    request.path = kHealthRoute;
    EXPECT_EQ(transport.handle_request(request).status, 200);
}

// 场景:隧道一直连不上边缘时用户关闭通道。
// 期望:stop() 很快返回,之后状态为已停止。
TEST(LineTransport, StopIsPromptWhileTunnelIsStarting) {
    Harness h;
    FakeCloudflared fake;
    fake.state()->ready = false;
    auto options = h.options();
    options.public_url.clear();
    options.tunnel = fake_tunnel(fake);
    options.tunnel.ready_timeout = std::chrono::seconds(30);
    Recorder recorder;  // 先于传输层声明:传输层析构(停机)时回调对象仍然有效
    LineTransport transport(options);
    transport.start(recorder.callbacks());
    ASSERT_TRUE(FakeLineServer::wait_until([&fake] { return fake.launch_count() == 1; }));
    const auto begin = std::chrono::steady_clock::now();
    transport.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(3));
    EXPECT_EQ(transport.status().state, LinkState::Stopped);
}

} // namespace
} // namespace acecode::im::line
