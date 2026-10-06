#pragma once

// 测试用:在 127.0.0.1 的随机端口上运行一个 Crow 应用,析构时停止。
// 用于 WebSocket 客户端、QQ 假网关、Telegram 假 Bot API 等本机端到端测试。

#include <crow.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <string>

namespace acecode::test {

class CrowTestServer {
public:
    explicit CrowTestServer(crow::SimpleApp& app) : app_(app) {
        app_.loglevel(crow::LogLevel::Warning);
        app_.bindaddr("127.0.0.1").port(0).concurrency(2).signal_clear();
        future_ = app_.run_async();
        app_.wait_for_server_start(std::chrono::seconds(5));
    }
    ~CrowTestServer() {
        app_.stop();
        if (future_.valid()) future_.wait();
    }
    CrowTestServer(const CrowTestServer&) = delete;
    CrowTestServer& operator=(const CrowTestServer&) = delete;

    std::uint16_t port() const { return app_.port(); }
    std::string http_base() const { return "http://127.0.0.1:" + std::to_string(port()); }
    std::string ws_base() const { return "ws://127.0.0.1:" + std::to_string(port()); }

private:
    crow::SimpleApp& app_;
    std::future<void> future_;
};

} // namespace acecode::test
