#include <gtest/gtest.h>

#include "channels/core/line_webhook_server.hpp"
#include "im/http.hpp"
#include "im/line/line_tunnel.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

// channels/core/line_webhook_server:LINE 回调端口(Crow)。
// 覆盖:只监听 127.0.0.1 的三条路由、原始请求体与签名头原样交给处理函数、其它路径 404、
// 停止后释放处理函数并可再次启动、固定端口。

namespace acecode::channels::core {
namespace {

using im::line::ListenerReply;
using im::line::ListenerRequest;

struct Recorder {
    std::mutex mu;
    std::vector<ListenerRequest> requests;

    std::size_t count() {
        std::lock_guard<std::mutex> lock(mu);
        return requests.size();
    }
    ListenerRequest at(std::size_t i) {
        std::lock_guard<std::mutex> lock(mu);
        return requests.at(i);
    }
};

im::line::ListenerHandler recording_handler(const std::shared_ptr<Recorder>& recorder) {
    return [recorder](const ListenerRequest& request) {
        {
            std::lock_guard<std::mutex> lock(recorder->mu);
            recorder->requests.push_back(request);
        }
        ListenerReply reply;
        reply.status = 200;
        reply.content_type = "application/json";
        reply.body = R"({"path":")" + request.path + R"("})";
        reply.headers = {{"Cache-Control", "no-store"}};
        return reply;
    };
}

im::HttpResponse send(const std::string& method, const std::string& url, const std::string& body = {},
                      std::vector<std::pair<std::string, std::string>> headers = {}) {
    im::HttpRequest request;
    request.method = method;
    request.url = url;
    request.body = body;
    request.headers = std::move(headers);
    request.timeout = std::chrono::seconds(5);
    request.use_proxy = false;
    return im::http_send(request);
}

// 场景:启动回调端口,依次发 webhook(带签名头,请求体含中文与换行)、健康检查、媒体请求。
// 期望:系统分配端口;处理函数拿到的方法、路径、原始请求体(逐字节一致)与签名头都正确;
// 处理函数的状态码、类型与头原样返回。
TEST(LineWebhookServer, ForwardsTheThreeRoutesVerbatim) {
    auto recorder = std::make_shared<Recorder>();
    LineWebhookServer server;
    std::string error;
    const auto port = server.start(0, recording_handler(recorder), &error);
    ASSERT_NE(port, 0) << error;
    EXPECT_EQ(server.port(), port);
    const auto base = "http://127.0.0.1:" + std::to_string(port);

    const std::string body = "{\"events\":[],\n\"text\":\"\xE4\xBD\xA0\xE5\xA5\xBD\"}";
    auto response = send("POST", base + "/line/webhook", body, {{"x-line-signature", "SIG=="}});
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, R"({"path":"/line/webhook"})");
    response = send("GET", base + "/line/health");
    EXPECT_EQ(response.status, 200);
    response = send("GET", base + "/line/media/tok123/a.png");
    EXPECT_EQ(response.status, 200);

    ASSERT_EQ(recorder->count(), 3u);
    EXPECT_EQ(recorder->at(0).method, "POST");
    EXPECT_EQ(recorder->at(0).path, "/line/webhook");
    EXPECT_EQ(recorder->at(0).body, body);
    EXPECT_EQ(recorder->at(0).signature, "SIG==");
    EXPECT_EQ(recorder->at(1).path, "/line/health");
    EXPECT_EQ(recorder->at(2).method, "GET");
    EXPECT_EQ(recorder->at(2).path, "/line/media/tok123/a.png");
    server.stop();
}

// 场景:请求不在白名单里的路径(API、主 Web 页面风格的路径、媒体多级路径),或用错方法。
// 期望:404 / 405,处理函数一次都不会被调用 —— 经隧道暴露的端口不能多出任何入口。
TEST(LineWebhookServer, EverythingElseIsRejected) {
    auto recorder = std::make_shared<Recorder>();
    LineWebhookServer server;
    std::string error;
    const auto port = server.start(0, recording_handler(recorder), &error);
    ASSERT_NE(port, 0) << error;
    const auto base = "http://127.0.0.1:" + std::to_string(port);
    EXPECT_EQ(send("GET", base + "/").status, 404);
    EXPECT_EQ(send("GET", base + "/api/sessions").status, 404);
    EXPECT_EQ(send("POST", base + "/api/pty", "{}").status, 404);
    EXPECT_EQ(send("GET", base + "/line/media/a/b/c").status, 404);
    EXPECT_NE(send("GET", base + "/line/webhook").status, 200);
    EXPECT_NE(send("POST", base + "/line/health", "{}").status, 200);
    EXPECT_EQ(recorder->count(), 0u);
}

// 场景:停止回调端口后再请求;随后再次启动。
// 期望:停止后端口不可达、port() 为 0、处理函数(及其捕获的对象)被释放;可以重新启动并正常工作。
TEST(LineWebhookServer, StopReleasesHandlerAndAllowsRestart) {
    auto recorder = std::make_shared<Recorder>();
    auto sentinel = std::make_shared<int>(7);
    std::weak_ptr<int> weak = sentinel;
    LineWebhookServer server;
    std::string error;
    auto handler = recording_handler(recorder);
    const auto port = server.start(
        0,
        [handler, sentinel](const ListenerRequest& request) { return handler(request); }, &error);
    sentinel.reset();
    ASSERT_NE(port, 0) << error;
    EXPECT_FALSE(weak.expired());
    server.stop();
    EXPECT_EQ(server.port(), 0);
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(send("GET", "http://127.0.0.1:" + std::to_string(port) + "/line/health").status, 0);

    const auto again = server.start(0, recording_handler(recorder), &error);
    ASSERT_NE(again, 0) << error;
    EXPECT_EQ(send("GET", "http://127.0.0.1:" + std::to_string(again) + "/line/health").status, 200);
}

// 场景:用户自己的反向代理指向固定端口,回调端口按该端口启动。
// 期望:返回的就是指定端口,并能在其上收到请求。
TEST(LineWebhookServer, HonorsFixedPort) {
    const auto wanted = im::line::pick_free_loopback_port();
    ASSERT_NE(wanted, 0);
    auto recorder = std::make_shared<Recorder>();
    LineWebhookServer server;
    std::string error;
    ASSERT_EQ(server.start(wanted, recording_handler(recorder), &error), wanted) << error;
    EXPECT_EQ(send("GET", "http://127.0.0.1:" + std::to_string(wanted) + "/line/health").status, 200);
}

// 场景:缺少处理函数。
// 期望:拒绝启动并给出原因,不会开出一个没人处理的端口。
TEST(LineWebhookServer, RefusesToStartWithoutHandler) {
    LineWebhookServer server;
    std::string error;
    EXPECT_EQ(server.start(0, {}, &error), 0);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(server.port(), 0);
}

} // namespace
} // namespace acecode::channels::core
