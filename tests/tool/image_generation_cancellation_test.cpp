#include <httplib.h>
#include <gtest/gtest.h>
#include "tool/image_generate/image_generation_client.hpp"
#include "network/proxy_resolver.hpp"
#include "utils/abandonable_call.hpp"
#include "utils/joining_thread.hpp"
#include "utils/scope_exit.hpp"
#include <condition_variable>
#include <future>
#include <mutex>

namespace {
using namespace acecode;
using namespace acecode::image_generation;

TEST(ImageGenerationCancellation, AbandonsBlockedHttpAndDrainsOwnedResponse) {
    // 场景:本机 HTTP 服务已收到图片请求但尚未回应时取消。期望调用方立即返回
    // aborted,释放其输入/取消标记后迟到请求安全结束;原手工 detached 未纳入退出等待。
    auto& resolver = network::proxy_resolver();
    const auto previous = resolver.config_snapshot();
    NetworkConfig direct = previous;
    direct.proxy_mode = "off";
    resolver.init(direct);
    ScopeExit restore([previous] { network::proxy_resolver().init(previous); });
    struct Gate {
        std::mutex mu;
        std::condition_variable cv;
        bool entered = false, released = false;
    };
    auto gate = std::make_shared<Gate>();
    httplib::Server server;
    server.Post("/v1/images/generations", [gate](const httplib::Request&, httplib::Response& response) {
        std::unique_lock<std::mutex> lock(gate->mu);
        gate->entered = true;
        gate->cv.notify_all();
        gate->cv.wait_for(lock, std::chrono::seconds(5), [gate] { return gate->released; });
        response.set_content(R"({"data":[{"b64_json":"aW1hZ2U="}]})", "application/json");
    });
    const auto port = server.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    JoiningThread serving([&server] { server.listen_after_bind(); });
    auto abort = std::make_shared<std::atomic<bool>>(false);
    auto promise = std::make_shared<std::promise<ImageResponse>>();
    auto response = promise->get_future();
    ImageRequest request;
    request.base_url = "http://127.0.0.1:" + std::to_string(port) + "/v1";
    request.model = "local-fixture";
    request.prompt = "local fixture";
    request.timeout_ms = 5000;
    JoiningThread calling([request, abort, promise] {
        try { promise->set_value(execute_image_request(request, abort.get())); }
        catch (...) { promise->set_exception(std::current_exception()); }
    });
    ScopeExit cleanup([&server, gate, abort] {
        abort->store(true);
        { std::lock_guard<std::mutex> lock(gate->mu); gate->released = true; }
        gate->cv.notify_all();
        wait_for_abandoned_work(std::chrono::seconds(6));
        server.stop();
    });
    {
        std::unique_lock<std::mutex> lock(gate->mu);
        ASSERT_TRUE(gate->cv.wait_for(lock, std::chrono::seconds(2), [gate] { return gate->entered; }));
    }
    abort->store(true);
    ASSERT_EQ(response.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    const auto result = response.get();
    EXPECT_TRUE(result.aborted);
    EXPECT_FALSE(result.ok);
    // 服务仍被门阻塞,因此这个结果不可能来自 HTTP 返回。
    { std::lock_guard<std::mutex> lock(gate->mu); EXPECT_FALSE(gate->released); }
}

TEST(ImageGenerationCancellation, PreCancelledRequestStartsNoNetworkWork) {
    // 场景:提交前取消。期望立即返回且不请求不存在的上游;
    // 原逻辑仍先启动后台 HTTP,浪费连接并延长退出收尾。
    ImageRequest request;
    request.base_url = "http://127.0.0.1:1/v1";
    request.timeout_ms = 5000;
    std::atomic<bool> abort{true};
    EXPECT_TRUE(execute_image_request(request, &abort).aborted);
}
} // namespace
