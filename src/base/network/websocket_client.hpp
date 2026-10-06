#pragma once

// 基于 libcurl WebSocket API(connect-only + curl_ws_send / curl_ws_recv)的
// 通用客户端,供 IM 通道(QQ 机器人网关)等长连接使用。
//
// 约定:
//   - 代理沿用 ProxyResolver 的判定,直连时显式禁用 libcurl 自己的环境变量代理,
//     与 cpr 调用点行为一致;TLS 保留 NoRevoke,证书校验交给 TLS 后端。
//   - receive() 只返回重组好的完整消息;ping 由 libcurl 自动回 pong,控制帧不上抛。
//   - 单条消息超过 max_message_bytes 视为协议错误,连接随即关闭。
//   - 一个线程 receive、另一个线程 send_text 是允许的(内部串行化 curl 调用);
//     abort() 可从任意线程调用,阻塞中的 receive/send 在 100ms 内返回。

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace acecode::network {

struct WebSocketMessage {
    bool binary = false;
    std::string data;
};

enum class WebSocketRecv {
    Message,  // out 中是一条完整消息
    Timeout,  // 超时前没有收到完整消息,连接仍然可用
    Closed,   // 对端关闭(close 帧或断开);close_code()/close_reason() 可读
    Error,    // 传输或协议错误;error 中是原因
};

struct WebSocketConnectOptions {
    std::string url;  // ws:// 或 wss://
    std::vector<std::pair<std::string, std::string>> headers;
    std::chrono::milliseconds connect_timeout{std::chrono::seconds(20)};
    std::size_t max_message_bytes = 4u * 1024u * 1024u;
    bool use_proxy = true;
};

class WebSocketClient {
public:
    WebSocketClient();
    ~WebSocketClient();
    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    // 建立连接并完成 HTTP Upgrade。失败时 error 含 curl 错误与 HTTP 状态码。
    bool connect(const WebSocketConnectOptions& options, std::string* error);
    bool send_text(const std::string& payload, std::chrono::milliseconds timeout,
                   std::string* error);
    // 发送一条二进制消息(飞书长连接的 protobuf 帧)。语义同 send_text。
    bool send_binary(const std::string& payload, std::chrono::milliseconds timeout,
                     std::string* error);
    WebSocketRecv receive(WebSocketMessage& out, std::chrono::milliseconds timeout,
                          std::string* error);
    // 尽力发送 close 帧后释放连接;重复调用无副作用。
    void close(std::uint16_t code = 1000, const std::string& reason = {});
    void abort();
    bool connected() const;
    // 对端 close 帧里的状态码;未收到 close 帧而断开时为 1006。
    int close_code() const;
    std::string close_reason() const;

private:
    bool send_frame(const std::string& payload, std::chrono::milliseconds timeout, std::string* error,
                    bool binary);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ws:// → http://、wss:// → https://,用于代理选择与 NO_PROXY 匹配。
std::string websocket_proxy_lookup_url(const std::string& ws_url);

} // namespace acecode::network
