#pragma once

// 飞书长连接会话状态机(纯逻辑,不做 IO)。
// 输入:收到的二进制帧、时间推进;输出:要立刻发送的帧(事件 ACK、心跳)、重组好的事件
// JSON、是否判定连接假死。另外提供重连等待时长的计算(服务端参数 + 本地退避)。
//
// 规则(官方 Go / Python SDK):
//   - 连上后立刻发一次心跳,之后每 PingInterval 秒一次;
//   - 任何帧都会刷新读超时;超过 2 × PingInterval + 5 秒没有收到任何帧即判定假死(NAT 断开、
//     休眠唤醒后 read 会永远阻塞,而心跳写入照样“成功”);
//   - CONTROL pong 若带 payload,是新的 ClientConfig,立即生效;服务端 ping 不回;
//   - DATA type=event 每帧都 ACK {"code":200}(包括我们不处理的事件类型),拆包的只对
//     凑齐最后一片的那一帧 ACK;type=card(旧版卡片回调)不回应直接丢弃。

#include "im/feishu/feishu_frame.hpp"
#include "im/feishu/feishu_protocol.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace acecode::im::feishu {

class LinkSession {
public:
    using Clock = std::chrono::steady_clock;

    struct Limits {
        std::chrono::milliseconds read_grace{std::chrono::seconds(5)};  // 读超时 = 2 × 心跳间隔 + grace
        std::chrono::milliseconds part_ttl{std::chrono::seconds(5)};    // 拆包缓存的存活时间(每片刷新)
        std::size_t max_parts = 64;            // 单条消息最多拆成几片
        std::size_t max_pending = 32;          // 同时在重组的消息数
        std::size_t max_total_bytes = 16u * 1024u * 1024u;  // 单条消息重组后的上限
    };

    struct Output {
        std::vector<std::string> frames;  // 立即发送的二进制帧(事件 ACK)
        std::vector<std::string> events;  // 完整的事件 JSON,按到达顺序
        bool config_updated = false;      // pong 带来了新的 ClientConfig
        std::string warning;              // 帧损坏等,只写日志(英文,不含正文)
    };

    struct Tick {
        std::optional<std::string> ping;  // 到期要发的心跳帧
        bool dead = false;                // 超过读超时没有收到任何帧
    };

    LinkSession() = default;
    explicit LinkSession(Limits limits) : limits_(limits) {}

    void set_config(const ClientConfig& config) { config_ = config; }
    const ClientConfig& config() const { return config_; }

    // WebSocket 握手成功后调用:立即安排一次心跳,重置读超时与拆包缓存。
    void on_connected(std::int32_t service_id, Clock::time_point now);
    void on_disconnected();
    Output on_message(const std::string& bytes, Clock::time_point now);
    Tick on_tick(Clock::time_point now);

    std::chrono::milliseconds ping_interval() const;
    std::chrono::milliseconds read_timeout() const;
    std::size_t pending_sets() const { return pending_.size(); }

private:
    struct Pending {
        std::vector<std::optional<std::string>> parts;
        std::size_t bytes = 0;
        Clock::time_point expires{};
    };

    std::optional<std::string> reassemble(const std::string& id, std::size_t sum, std::size_t seq,
                                          std::string payload, Clock::time_point now, std::string* warning);
    void sweep(Clock::time_point now);

    Limits limits_;
    ClientConfig config_;
    std::int32_t service_ = 0;
    bool connected_ = false;
    Clock::time_point next_ping_{};
    Clock::time_point last_receive_{};
    std::map<std::string, Pending> pending_;
};

// 下一次连接前的等待。failures = 自上次成功连接以来连续失败的次数(刚掉线、还没重试时为 0)。
//   - 还没拿到过服务端参数(server_config=false):只用本地退避 backoff[failures];
//   - 刚掉线(failures == 0):在 [0, ReconnectNonce] 秒里随机抖动(jitter ∈ [0,1)),与本地退避取大;
//   - 之后每次:ReconnectInterval 秒,与本地退避取大(服务端参数优先,SDK 注释明确要求)。
std::chrono::milliseconds reconnect_delay(const ClientConfig& config, bool server_config, std::size_t failures,
                                          const std::vector<std::chrono::milliseconds>& backoff, double jitter);

// ReconnectCount >= 0 时,连续失败达到该次数后停止重连(-1 = 无限)。
bool reconnect_exhausted(const ClientConfig& config, bool server_config, std::size_t failures);

} // namespace acecode::im::feishu
