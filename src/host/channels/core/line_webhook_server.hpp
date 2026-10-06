#pragma once

// LINE 回调端口的 Crow 实现(im::line::WebhookListener)。
//
// 只监听 127.0.0.1(Cloudflare 隧道或用户的反向代理在本机转发进来),只注册三条路由:
//   POST /line/webhook          → 原始请求体 + X-Line-Signature 交给传输层(校验签名、入队、立即回 200)
//   GET  /line/health           → 公网自检
//   GET  /line/media/<令牌>/<名> → 出站图片的临时链接
// 其余一律 404,不加 CORS 头。它是独立的 Crow 应用,绝不复用 ACECode 主 Web 端口
// (本机请求在主端口免 token,经隧道进来的请求也是本机地址)。
//
// 一个传输层实例独占一个 LineWebhookServer;stop() 之后可以再次 start()。

#include "im/line/line_webhook.hpp"

#include <memory>

namespace acecode::channels::core {

class LineWebhookServer final : public im::line::WebhookListener {
public:
    LineWebhookServer();
    ~LineWebhookServer() override;
    LineWebhookServer(const LineWebhookServer&) = delete;
    LineWebhookServer& operator=(const LineWebhookServer&) = delete;

    std::uint16_t start(std::uint16_t port, im::line::ListenerHandler handler, std::string* error) override;
    void stop() override;
    std::uint16_t port() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acecode::channels::core
