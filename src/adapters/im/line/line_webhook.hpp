#pragma once

// LINE 回调接收端口的抽象接口。LINE 只能用 webhook 推送消息,ACECode 必须在本机开一个
// HTTP 端口,再经 Cloudflare 隧道或用户自己的反向代理暴露到公网。
//
// 分层:adapters 不能依赖 Crow(R7),所以这里只定义接口;Crow 实现在
// host/channels/core/line_webhook_server.{hpp,cpp}(channels::core::LineWebhookServer),
// 由核心创建后经 LineTransportOptions::listener 交给传输层。
//
// 约定(实现必须遵守):
//   - 只监听 127.0.0.1,绝不监听 0.0.0.0;不能是 ACECode 主 Web 端口(本机请求在那边免 token);
//   - 只提供 POST /line/webhook、GET /line/health、GET /line/media/<令牌>/<文件名>,
//     其余一律 404,不加任何 CORS 头;
//   - 请求体原样交给处理函数(签名按原始字节计算,不能先解析再序列化);
//   - 处理函数在 HTTP 线程上调用,必须很快返回(传输层只校验签名并入队)。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace acecode::im::line {

inline constexpr const char* kWebhookRoute = "/line/webhook";
inline constexpr const char* kHealthRoute = "/line/health";
// 超过 1 MiB 的请求体直接拒绝(LINE 的 webhook 远小于此)。
inline constexpr std::size_t kMaxWebhookBodyBytes = 1024 * 1024;

struct ListenerRequest {
    std::string method;     // "GET" / "POST" / "HEAD"
    std::string path;       // 不含查询串,未解码
    std::string body;       // 原始字节
    std::string signature;  // X-Line-Signature(头名大小写不敏感)
};

struct ListenerReply {
    int status = 404;
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

using ListenerHandler = std::function<ListenerReply(const ListenerRequest&)>;

class WebhookListener {
public:
    virtual ~WebhookListener() = default;
    // 在 127.0.0.1:port 上开始监听(port 为 0 时由系统分配)。成功返回实际端口;
    // 失败返回 0 并写 error。已在运行时先停掉再重新监听。
    virtual std::uint16_t start(std::uint16_t port, ListenerHandler handler, std::string* error) = 0;
    // 阻塞到服务线程全部退出;返回后不再调用 handler,并释放它。可重复调用。
    virtual void stop() = 0;
    // 正在监听的端口;未运行时为 0。
    virtual std::uint16_t port() const = 0;
};

} // namespace acecode::im::line
