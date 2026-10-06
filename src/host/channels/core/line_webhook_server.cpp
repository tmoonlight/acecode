#include "channels/core/line_webhook_server.hpp"

#include "utils/joining_thread.hpp"
#include "utils/logger.hpp"

#include <crow.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

namespace acecode::channels::core {
namespace {

// 路由与 LineWebhookServer 共享的处理函数槽。路由只捕获它的 shared_ptr,不捕获服务对象本身;
// stop() 在服务线程全部退出后清空它。
struct HandlerSlot {
    std::mutex mu;
    im::line::ListenerHandler handler;
};

crow::response dispatch(HandlerSlot& slot, const char* method, std::string path, const crow::request& req) {
    im::line::ListenerHandler handler;
    {
        std::lock_guard<std::mutex> lock(slot.mu);
        handler = slot.handler;
    }
    if (!handler) return crow::response(503);
    im::line::ListenerRequest request;
    request.method = method;
    request.path = std::move(path);
    request.body = req.body;  // 原始字节;签名按它计算
    request.signature = req.get_header_value("X-Line-Signature");
    im::line::ListenerReply reply;
    try {
        reply = handler(request);
    } catch (const std::exception& e) {
        LOG_WARN(std::string("[channels/line] webhook handler failed: ") + e.what());
        reply = im::line::ListenerReply{};
        reply.status = 500;
        reply.body = "error";
    }
    crow::response res(reply.status);
    res.set_header("Content-Type", reply.content_type);
    for (const auto& [name, value] : reply.headers) res.set_header(name, value);
    res.body = std::move(reply.body);
    return res;
}

// exited 指向 Impl 的成员;Impl 在 join 服务线程之前不会销毁。
void run_app(crow::SimpleApp* app, std::atomic<bool>* exited) {
    try {
        app->run();
    } catch (const std::exception& e) {
        LOG_WARN(std::string("[channels/line] webhook listener stopped: ") + e.what());
    } catch (...) {
        LOG_WARN("[channels/line] webhook listener stopped unexpectedly");
    }
    exited->store(true);
}

} // namespace

struct LineWebhookServer::Impl {
    std::mutex ops_mu;  // 串行化 start / stop
    std::unique_ptr<crow::SimpleApp> app;
    acecode::JoiningThread server;
    std::shared_ptr<HandlerSlot> slot;
    std::atomic<bool> exited{false};
    std::atomic<std::uint16_t> port{0};

    void stop_locked() {
        if (app) app->stop();
        if (server.joinable()) server.join();
        if (slot) {
            std::lock_guard<std::mutex> lock(slot->mu);
            slot->handler = nullptr;
        }
        slot.reset();
        app.reset();
        port = 0;
    }
};

LineWebhookServer::LineWebhookServer() : impl_(std::make_unique<Impl>()) {}

LineWebhookServer::~LineWebhookServer() { stop(); }

std::uint16_t LineWebhookServer::start(std::uint16_t port, im::line::ListenerHandler handler, std::string* error) {
    std::lock_guard<std::mutex> lock(impl_->ops_mu);
    impl_->stop_locked();
    if (!handler) {
        if (error) *error = "缺少回调处理函数";
        return 0;
    }
    auto slot = std::make_shared<HandlerSlot>();
    slot->handler = std::move(handler);
    auto app = std::make_unique<crow::SimpleApp>();

    CROW_ROUTE((*app), "/line/webhook").methods(crow::HTTPMethod::Post)([slot](const crow::request& req) {
        return dispatch(*slot, "POST", im::line::kWebhookRoute, req);
    });
    CROW_ROUTE((*app), "/line/health").methods(crow::HTTPMethod::Get)([slot](const crow::request& req) {
        return dispatch(*slot, "GET", im::line::kHealthRoute, req);
    });
    CROW_ROUTE((*app), "/line/media/<string>/<string>")
        .methods(crow::HTTPMethod::Get)([slot](const crow::request& req, const std::string& token,
                                               const std::string& name) {
            return dispatch(*slot, "GET", std::string("/line/media/") + token + "/" + name, req);
        });
    CROW_CATCHALL_ROUTE((*app))([] { return crow::response(404); });

    // 只监听回环地址;两个工作线程(LINE 会并发取图片与预览);signal_clear 避免 Crow 接管进程的 Ctrl+C / SIGTERM。
    app->bindaddr("127.0.0.1").port(port).concurrency(3).signal_clear();
    impl_->exited = false;
    auto* raw = app.get();
    impl_->server = acecode::JoiningThread(&run_app, raw, &impl_->exited);
    const auto waited = raw->wait_for_server_start(std::chrono::seconds(5));
    std::uint16_t bound = 0;
    if (waited == std::cv_status::no_timeout && !impl_->exited.load() && raw->is_bound()) {
        try {
            bound = raw->port();
        } catch (...) {
            bound = 0;
        }
    }
    if (bound == 0 || (port != 0 && bound != port)) {
        raw->stop();
        if (impl_->server.joinable()) impl_->server.join();
        {
            std::lock_guard<std::mutex> slot_lock(slot->mu);
            slot->handler = nullptr;
        }
        if (error) {
            *error = port != 0 ? "端口 " + std::to_string(port) + " 已被占用或无法监听" : std::string("无法监听本机端口");
        }
        LOG_WARN("[channels/line] webhook listener failed to start on 127.0.0.1:" + std::to_string(port));
        return 0;
    }
    impl_->app = std::move(app);
    impl_->slot = std::move(slot);
    impl_->port = bound;
    return bound;
}

void LineWebhookServer::stop() {
    std::lock_guard<std::mutex> lock(impl_->ops_mu);
    impl_->stop_locked();
}

std::uint16_t LineWebhookServer::port() const {
    // 服务线程意外退出(端口被系统收回等)时视为未运行,传输层会重新监听。
    if (impl_->exited.load()) return 0;
    return impl_->port.load();
}

} // namespace acecode::channels::core
