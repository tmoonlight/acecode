#include <gtest/gtest.h>

#include "im/dingtalk/dingtalk_transport.hpp"
#include "test_support/im/fake_dingtalk_server.hpp"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <vector>

// im/dingtalk/dingtalk_transport:钉钉传输层的连接与收消息端到端测试。本机假开放平台提供
// Stream 注册、一次性 ticket 的 WebSocket 网关与机器人回调推送;覆盖连接、先回执后处理、
// 两层去重、ping 回显、断开指令立即重连、空闲重连、致命错误停止重试与附件下载。

namespace acecode::im::dingtalk {
namespace {

using Server = test::FakeDingTalkServer;

DingTalkTransportOptions local_options(Server& server) {
    DingTalkTransportOptions options;
    options.api.client_id = server.client_id;
    options.api.client_secret = server.client_secret;
    options.api.api_base = server.base();
    options.api.oapi_base = server.base();
    options.api.use_proxy = false;
    options.backoff_base = std::chrono::milliseconds(50);
    options.backoff_cap = std::chrono::milliseconds(200);
    options.jitter = false;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.healthy_after = std::chrono::milliseconds(0);
    options.throttle_delay = std::chrono::milliseconds(100);
    options.qps_delay = std::chrono::milliseconds(50);
    return options;
}

struct Recorder {
    std::mutex mu;
    std::vector<Inbound> inbound;
    std::vector<TransportStatus> statuses;
    std::function<void(const Inbound&)> hook;  // 在传输层分发线程上、记录之前调用
    TransportCallbacks callbacks() {
        TransportCallbacks cb;
        cb.on_inbound = [this](Inbound in) {
            if (hook) hook(in);
            std::lock_guard<std::mutex> lock(mu);
            inbound.push_back(std::move(in));
        };
        cb.on_status = [this](const TransportStatus& s) {
            std::lock_guard<std::mutex> lock(mu);
            statuses.push_back(s);
        };
        return cb;
    }
    std::size_t inbound_count() {
        std::lock_guard<std::mutex> lock(mu);
        return inbound.size();
    }
    Inbound inbound_at(std::size_t i) {
        std::lock_guard<std::mutex> lock(mu);
        return inbound.at(i);
    }
};

bool wait_connected(DingTalkTransport& transport) {
    return Server::wait_until([&transport] { return transport.status().state == LinkState::Connected; });
}

// 已连接且假网关已登记第 connections 条连接(之后 push 一定落在这条连接上)。
bool wait_ready(DingTalkTransport& transport, Server& server, int connections = 1) {
    return wait_connected(transport) &&
           Server::wait_until([&server, connections] { return server.connections() >= connections; });
}

// 场景:开启钉钉通道,Stream 注册拿到 ticket 后连上网关,平台推来一条单聊消息。
// 期望:注册请求带 clientId 与机器人回调订阅;握手使用刚发的 ticket;状态变为已连接;
// 入站消息地址为 Client ID + staffId;该帧以 code 200 回执;停止后状态为已停止。
TEST(DingTalkTransport, ConnectsAndReceivesPrivateMessage) {
    Server server;
    Recorder recorder;
    DingTalkTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_ready(transport, server));
    EXPECT_EQ(transport.status().account, server.client_id);
    const auto open = server.requests_to("/v1.0/gateway/connections/open");
    ASSERT_EQ(open.size(), 1u);
    EXPECT_EQ(open[0].body.value("clientId", ""), server.client_id);
    EXPECT_EQ(server.used_tickets(), std::vector<std::string>{"ticket-1"});

    server.push_callback("h1", server.private_text("m1", "staff01", u8" 你好 "));
    ASSERT_TRUE(Server::wait_until([&recorder] { return recorder.inbound_count() == 1; }));
    const auto inbound = recorder.inbound_at(0);
    EXPECT_EQ(inbound.address.platform, "dingtalk");
    EXPECT_EQ(inbound.address.account, server.client_id);
    EXPECT_EQ(inbound.address.chat, "staff01");
    EXPECT_EQ(inbound.text, u8"你好");
    EXPECT_EQ(inbound.reply_context.value("sessionWebhook", ""), server.webhook("S1"));
    ASSERT_TRUE(Server::wait_until([&server] { return server.has_ack("h1"); }));
    for (const auto& ack : server.acks()) {
        if (ack["headers"].value("messageId", "") == "h1") EXPECT_EQ(ack.value("code", 0), 200);
    }
    transport.stop();
    EXPECT_EQ(transport.status().state, LinkState::Stopped);
}

// 场景:上层处理一条消息很慢(处理函数阻塞);处理期间平台又发来 SYSTEM ping。
// 期望:回执在处理完成之前就已送达平台(先回执后处理);ping 照样被及时回执 —— 读线程不被处理阻塞。
// 回归:若在读线程上处理消息,慢处理会让平台收不到回执而重投,并因 ping 无回应判定连接失效。
TEST(DingTalkTransport, AcknowledgesBeforeProcessingAndKeepsReading) {
    Server server;
    // 回调里用到的对象都声明在传输层之前,保证传输层先析构(先停线程)。
    Recorder recorder;
    std::mutex gate_mu;
    std::condition_variable gate_cv;
    bool release = false;
    std::atomic<bool> acked_while_blocked{false};
    DingTalkTransport transport(local_options(server));
    recorder.hook = [&server, &gate_mu, &gate_cv, &release, &acked_while_blocked](const Inbound&) {
        acked_while_blocked = Server::wait_until([&server] { return server.has_ack("h1"); });
        std::unique_lock<std::mutex> lock(gate_mu);
        gate_cv.wait_for(lock, std::chrono::seconds(5), [&release] { return release; });
    };
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_ready(transport, server));
    server.push_callback("h1", server.private_text("m1", "staff01", "slow"));
    ASSERT_TRUE(Server::wait_until([&acked_while_blocked] { return acked_while_blocked.load(); }));
    server.push_ping("p1", "op-1");
    EXPECT_TRUE(Server::wait_until([&server] { return server.has_ack("p1"); }));
    EXPECT_EQ(recorder.inbound_count(), 0u);  // 处理函数仍阻塞着
    {
        std::lock_guard<std::mutex> lock(gate_mu);
        release = true;
    }
    gate_cv.notify_all();
    EXPECT_TRUE(Server::wait_until([&recorder] { return recorder.inbound_count() == 1; }));
    transport.stop();
}

// 场景:平台重投同一条消息(帧头 messageId 换新、msgId 不变),以及同一帧被重复推送。
// 期望:每一帧都回执(否则平台会继续重投);上层只收到一次。
TEST(DingTalkTransport, DeduplicatesRedeliveryOnBothIds) {
    Server server;
    Recorder recorder;
    DingTalkTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_ready(transport, server));
    const auto data = server.private_text("m1", "staff01", "hi");
    server.push_callback("h1", data);
    server.push_callback("h2", data);
    server.push_callback("h1", data);
    server.push_callback("h3", server.private_text("m2", "staff01", "second"));
    ASSERT_TRUE(Server::wait_until([&recorder] { return recorder.inbound_count() == 2; }));
    ASSERT_TRUE(Server::wait_until([&server] { return server.has_ack("h2") && server.has_ack("h3"); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(recorder.inbound_count(), 2u);
    EXPECT_EQ(recorder.inbound_at(1).message_id, "m2");
}

// 场景:平台发来 SYSTEM ping,data 里带 opaque。
// 期望:回执的 messageId 相同,data 逐字回显 opaque。
TEST(DingTalkTransport, AnswersPingWithOpaqueEcho) {
    Server server;
    Recorder recorder;
    DingTalkTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_ready(transport, server));
    server.push_ping("p9", "opaque-123");
    ASSERT_TRUE(Server::wait_until([&server] { return server.has_ack("p9"); }));
    for (const auto& ack : server.acks()) {
        if (ack["headers"].value("messageId", "") != "p9") continue;
        EXPECT_EQ(ack.value("code", 0), 200);
        EXPECT_EQ(nlohmann::json::parse(ack.value("data", "{}")).value("opaque", ""), "opaque-123");
    }
}

// 场景:平台发来 SYSTEM disconnect(负载切换)。退避基数故意设成 5 秒。
// 期望:先回执,再关闭并立即(不等 5 秒退避)用新的 ticket 重新注册、重连,回到已连接。
// 回归:ticket 一次性,复用旧 ticket 会握手失败。
TEST(DingTalkTransport, DisconnectReconnectsImmediatelyWithFreshTicket) {
    Server server;
    auto options = local_options(server);
    options.backoff_base = std::chrono::seconds(5);
    options.backoff_cap = std::chrono::seconds(5);
    Recorder recorder;
    DingTalkTransport transport(options);
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_ready(transport, server));
    server.push_disconnect("d1");
    ASSERT_TRUE(Server::wait_until([&server] { return server.connections() == 2; }, std::chrono::seconds(2)));
    EXPECT_TRUE(server.has_ack("d1"));
    EXPECT_EQ(server.used_tickets(), (std::vector<std::string>{"ticket-1", "ticket-2"}));
    ASSERT_TRUE(wait_connected(transport));
    server.push_callback("h1", server.private_text("m1", "staff01", "after reconnect"));
    EXPECT_TRUE(Server::wait_until([&recorder] { return recorder.inbound_count() == 1; }));
}

// 场景:网关直接关闭连接(网络抖动、服务端重启)。
// 期望:按退避等待后重新注册并重连,回到已连接。
TEST(DingTalkTransport, ReconnectsAfterServerClose) {
    Server server;
    Recorder recorder;
    DingTalkTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_ready(transport, server));
    server.close_connection(1001);
    ASSERT_TRUE(Server::wait_until([&server] { return server.connections() == 2; }));
    EXPECT_TRUE(wait_connected(transport));
    EXPECT_EQ(server.issued_tickets().size(), 2u);
}

// 场景:连接建立后平台一直不发任何帧(半开连接);空闲阈值设为 300 毫秒。
// 期望:判定连接失效并主动重连。
TEST(DingTalkTransport, ReplacesIdleConnection) {
    Server server;
    auto options = local_options(server);
    options.idle_timeout = std::chrono::milliseconds(300);
    Recorder recorder;
    DingTalkTransport transport(options);
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_ready(transport, server));
    EXPECT_TRUE(Server::wait_until([&server] { return server.connections() >= 2; }));
}

// 场景:Stream 注册第一次返回 500(平台临时故障),之后恢复。
// 期望:状态先是重试中,随后自动连上;共注册两次。
TEST(DingTalkTransport, RetriesTemporaryRegistrationFailure) {
    Server server;
    std::atomic<int> calls{0};
    server.open_handler = [&calls](const Server::Request&) {
        if (calls++ == 0) return Server::Reply{500, {{"code", "ServiceUnavailable"}, {"message", "busy"}}};
        return Server::Reply{0, nullptr};
    };
    Recorder recorder;
    DingTalkTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_ready(transport, server));
    EXPECT_EQ(server.requests_to("/v1.0/gateway/connections/open").size(), 2u);
    bool retried = false;
    {
        std::lock_guard<std::mutex> lock(recorder.mu);
        for (const auto& s : recorder.statuses) retried = retried || s.state == LinkState::Retrying;
    }
    EXPECT_TRUE(retried);
}

// 场景:Client Secret 错误,Stream 注册返回 401 authFailed。
// 期望:状态变为失败且停止重试,不建立 WebSocket;原因提示核对凭据且不含密钥原文;之后不再注册。
TEST(DingTalkTransport, InvalidCredentialsStopRetrying) {
    Server server;
    auto options = local_options(server);
    options.api.client_secret = "wrong-secret-value-999";
    Recorder recorder;
    DingTalkTransport transport(options);
    transport.start(recorder.callbacks());
    ASSERT_TRUE(Server::wait_until([&transport] { return transport.status().state == LinkState::Failed; }));
    const auto status = transport.status();
    EXPECT_TRUE(status.retry_stopped);
    EXPECT_NE(status.detail.find("Client Secret"), std::string::npos) << status.detail;
    EXPECT_EQ(status.detail.find("wrong-secret-value-999"), std::string::npos);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(server.connections(), 0);
    EXPECT_EQ(server.requests_to("/v1.0/gateway/connections/open").size(), 1u);
}

// 场景:应用未发布或机器人未开 Stream 模式,Stream 注册返回 400。
// 期望:失败并停止重试(用户在钉钉后台处理后重新打开开关即可),原因提到发布与 Stream 模式。
TEST(DingTalkTransport, UnpublishedAppStopsRetrying) {
    Server server;
    server.open_handler = [](const Server::Request&) {
        return Server::Reply{400, {{"code", "invalidParameter"}, {"message", "app not published"}}};
    };
    Recorder recorder;
    DingTalkTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(Server::wait_until([&transport] { return transport.status().state == LinkState::Failed; }));
    EXPECT_TRUE(transport.status().retry_stopped);
    EXPECT_NE(transport.status().detail.find(u8"Stream 模式"), std::string::npos);
}

// 场景:群里 @机器人 发消息,开头带着机器人的 @名字。
// 期望:群地址按 conversationId + staffId;mentioned=true;@名字已去掉。
TEST(DingTalkTransport, ReceivesGroupMention) {
    Server server;
    Recorder recorder;
    DingTalkTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_ready(transport, server));
    server.push_callback("h1", server.group_text("g1", "cidGROUP", "staff02", u8"@ACE /status"));
    ASSERT_TRUE(Server::wait_until([&recorder] { return recorder.inbound_count() == 1; }));
    const auto inbound = recorder.inbound_at(0);
    EXPECT_EQ(inbound.address.kind, ChatKind::Group);
    EXPECT_EQ(inbound.address.chat, "cidGROUP");
    EXPECT_EQ(inbound.address.sender, "staff02");
    EXPECT_TRUE(inbound.mentioned);
    EXPECT_EQ(inbound.text, "/status");
}

// 场景:用户发来一张图片,核心下载它;另一张图片超过本地下载上限。
// 期望:用 downloadCode 换地址后下载成功;超限时返回 false 并给出中文原因。
TEST(DingTalkTransport, DownloadsPictureAttachment) {
    Server server;
    auto options = local_options(server);
    options.max_download_bytes = 100;
    Recorder recorder;
    DingTalkTransport transport(options);
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_ready(transport, server));
    auto data = server.private_text("m1", "staff01", "");
    data.erase("text");
    data["msgtype"] = "picture";
    data["content"] = {{"downloadCode", "code1"}};
    server.push_callback("h1", data);
    ASSERT_TRUE(Server::wait_until([&recorder] { return recorder.inbound_count() == 1; }));
    const auto inbound = recorder.inbound_at(0);
    ASSERT_EQ(inbound.attachments.size(), 1u);
    const auto dest = std::filesystem::path(testing::TempDir()) / "acecode-dingtalk-pic.bin";
    std::string error;
    ASSERT_TRUE(transport.download(inbound.attachments[0], dest, &error)) << error;
    EXPECT_EQ(std::filesystem::file_size(dest), 16u);

    auto big = inbound.attachments[0];
    big.remote_ref = "big";
    EXPECT_FALSE(transport.download(big, dest, &error));
    EXPECT_NE(error.find(u8"超过"), std::string::npos);
    std::filesystem::remove(dest);
}

} // namespace
} // namespace acecode::im::dingtalk
