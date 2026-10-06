#include <gtest/gtest.h>

#include "network/websocket_client.hpp"
#include "test_support/network/crow_test_server.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

// network/websocket_client 的本机端到端测试:用 Crow 起一个 WebSocket 服务,
// 覆盖收发、对端关闭、超大消息、超时、中止等 QQ 网关会遇到的情形。
// 测试一律关闭代理(use_proxy=false),避免本机系统代理影响 127.0.0.1 连接。

namespace acecode::network {
namespace {

// 回显服务:普通文本原样返回;"close-4008" 让服务端以 4008 关闭;
// "big:N" 让服务端回一条 N 字节的文本。
struct EchoApp {
    crow::SimpleApp app;
    EchoApp() {
        CROW_WEBSOCKET_ROUTE(app, "/ws")
            .onmessage([](crow::websocket::connection& conn, const std::string& data, bool binary) {
                if (data == "close-4008") {
                    conn.close("rate limited", 4008);
                    return;
                }
                if (data.rfind("big:", 0) == 0) {
                    conn.send_text(std::string(std::stoul(data.substr(4)), 'x'));
                    return;
                }
                if (binary) conn.send_binary(data);
                else conn.send_text(data);
            });
    }
};

WebSocketConnectOptions local_options(const test::CrowTestServer& server) {
    WebSocketConnectOptions options;
    options.url = server.ws_base() + "/ws";
    options.use_proxy = false;
    options.connect_timeout = std::chrono::seconds(5);
    return options;
}

// 场景:连上本机服务后发送一条含中文的文本。
// 期望:收到逐字节相同的回显,连接保持可用。
TEST(WebSocketClient, EchoesUtf8TextMessage) {
    EchoApp echo;
    test::CrowTestServer server(echo.app);
    WebSocketClient client;
    std::string error;
    ASSERT_TRUE(client.connect(local_options(server), &error)) << error;
    const std::string payload = u8"你好,WebSocket 🚀";
    ASSERT_TRUE(client.send_text(payload, std::chrono::seconds(5), &error)) << error;
    WebSocketMessage message;
    ASSERT_EQ(client.receive(message, std::chrono::seconds(5), &error), WebSocketRecv::Message) << error;
    EXPECT_FALSE(message.binary);
    EXPECT_EQ(message.data, payload);
    EXPECT_TRUE(client.connected());
}

// 场景:服务端回一条 200000 字节的消息,远大于单次 curl_ws_recv 的 64KB 缓冲。
// 期望:客户端把多次读到的片段拼成一条完整消息返回。
TEST(WebSocketClient, ReassemblesMessageLargerThanReceiveBuffer) {
    EchoApp echo;
    test::CrowTestServer server(echo.app);
    WebSocketClient client;
    std::string error;
    ASSERT_TRUE(client.connect(local_options(server), &error)) << error;
    ASSERT_TRUE(client.send_text("big:200000", std::chrono::seconds(5), &error)) << error;
    WebSocketMessage message;
    ASSERT_EQ(client.receive(message, std::chrono::seconds(10), &error), WebSocketRecv::Message) << error;
    EXPECT_EQ(message.data.size(), 200000u);
}

// 场景:客户端把单条消息上限设为 2048 字节,服务端却发来 10000 字节。
// 期望:receive 报错“超出上限”并主动关闭连接,而不是无限占用内存。
TEST(WebSocketClient, RejectsMessageAboveConfiguredLimit) {
    EchoApp echo;
    test::CrowTestServer server(echo.app);
    WebSocketClient client;
    auto options = local_options(server);
    options.max_message_bytes = 2048;
    std::string error;
    ASSERT_TRUE(client.connect(options, &error)) << error;
    ASSERT_TRUE(client.send_text("big:10000", std::chrono::seconds(5), &error)) << error;
    WebSocketMessage message;
    EXPECT_EQ(client.receive(message, std::chrono::seconds(5), &error), WebSocketRecv::Error);
    EXPECT_NE(error.find("exceeds limit"), std::string::npos) << error;
    EXPECT_FALSE(client.connected());
}

// 场景:服务端用 4008(QQ 网关的限频关闭码)和原因文本关闭连接。
// 期望:receive 返回 Closed,并能读到 4008 与原因,供上层决定退避策略。
TEST(WebSocketClient, ReportsPeerCloseCodeAndReason) {
    EchoApp echo;
    test::CrowTestServer server(echo.app);
    WebSocketClient client;
    std::string error;
    ASSERT_TRUE(client.connect(local_options(server), &error)) << error;
    ASSERT_TRUE(client.send_text("close-4008", std::chrono::seconds(5), &error)) << error;
    WebSocketMessage message;
    ASSERT_EQ(client.receive(message, std::chrono::seconds(5), &error), WebSocketRecv::Closed) << error;
    EXPECT_EQ(client.close_code(), 4008);
    EXPECT_EQ(client.close_reason(), "rate limited");
    EXPECT_FALSE(client.connected());
}

// 场景:连接空闲,receive 只等 200ms。
// 期望:返回 Timeout 且连接仍然可用,之后照常收发。
TEST(WebSocketClient, TimeoutKeepsConnectionUsable) {
    EchoApp echo;
    test::CrowTestServer server(echo.app);
    WebSocketClient client;
    std::string error;
    ASSERT_TRUE(client.connect(local_options(server), &error)) << error;
    WebSocketMessage message;
    EXPECT_EQ(client.receive(message, std::chrono::milliseconds(200), &error), WebSocketRecv::Timeout);
    EXPECT_TRUE(client.connected());
    ASSERT_TRUE(client.send_text("after-timeout", std::chrono::seconds(5), &error)) << error;
    ASSERT_EQ(client.receive(message, std::chrono::seconds(5), &error), WebSocketRecv::Message) << error;
    EXPECT_EQ(message.data, "after-timeout");
}

// 场景:receive 以 10 秒超时阻塞时,另一个线程调用 abort()(对应停止通道)。
// 期望:receive 在 2 秒内返回 Error,不会等满 10 秒。
TEST(WebSocketClient, AbortWakesBlockedReceive) {
    EchoApp echo;
    test::CrowTestServer server(echo.app);
    WebSocketClient client;
    std::string error;
    ASSERT_TRUE(client.connect(local_options(server), &error)) << error;
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        client.abort();
    });
    const auto start = std::chrono::steady_clock::now();
    WebSocketMessage message;
    EXPECT_EQ(client.receive(message, std::chrono::seconds(10), &error), WebSocketRecv::Error);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
    stopper.join();
}

// 场景:连接建立后服务端进程直接停掉(对应网络断开或网关重启)。
// 期望:receive 不会卡住也不会误报消息,而是报告连接已断开,状态变为未连接。
TEST(WebSocketClient, ServerShutdownIsReportedAsDisconnect) {
    auto echo = std::make_unique<EchoApp>();
    auto server = std::make_unique<test::CrowTestServer>(echo->app);
    WebSocketClient client;
    std::string error;
    ASSERT_TRUE(client.connect(local_options(*server), &error)) << error;
    server.reset();
    WebSocketMessage message;
    const auto result = client.receive(message, std::chrono::seconds(5), &error);
    EXPECT_TRUE(result == WebSocketRecv::Closed || result == WebSocketRecv::Error);
    EXPECT_FALSE(client.connected());
}

// 场景:连接一个没有 WebSocket 路由的路径,以及一个没有服务监听的端口。
// 期望:connect 返回 false 并给出非空错误,状态为未连接。
TEST(WebSocketClient, ConnectFailuresAreReported) {
    EchoApp echo;
    test::CrowTestServer server(echo.app);
    WebSocketClient client;
    std::string error;
    auto options = local_options(server);
    options.url = server.ws_base() + "/missing";
    EXPECT_FALSE(client.connect(options, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(client.connected());
    error.clear();
    options.url = "ws://127.0.0.1:1/ws";
    options.connect_timeout = std::chrono::seconds(2);
    EXPECT_FALSE(client.connect(options, &error));
    EXPECT_FALSE(error.empty());
}

// 场景:未连接时调用 send_text。
// 期望:直接返回 false 并说明未连接,不崩溃。
TEST(WebSocketClient, SendWithoutConnectionFails) {
    WebSocketClient client;
    std::string error;
    EXPECT_FALSE(client.send_text("x", std::chrono::milliseconds(100), &error));
    EXPECT_NE(error.find("not connected"), std::string::npos) << error;
}

// 场景:代理选择需要把 ws/wss 地址换成 http/https 地址再查 NO_PROXY。
// 期望:scheme 按大小写不敏感替换,其余部分原样保留;非 ws 地址不变。
TEST(WebSocketClient, ProxyLookupUrlMapsWebSocketSchemes) {
    EXPECT_EQ(websocket_proxy_lookup_url("wss://api.sgroup.qq.com/websocket"),
              "https://api.sgroup.qq.com/websocket");
    EXPECT_EQ(websocket_proxy_lookup_url("WS://127.0.0.1:9/x"), "http://127.0.0.1:9/x");
    EXPECT_EQ(websocket_proxy_lookup_url("https://example.com"), "https://example.com");
}

} // namespace
} // namespace acecode::network
