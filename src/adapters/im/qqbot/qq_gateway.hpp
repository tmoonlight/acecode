#pragma once

// QQ 网关会话状态机(纯逻辑,不做 IO)。
// 输入:收到的帧、时间推进、连接关闭码;输出:要发送的帧、派发的事件与连接指令。
// 协议:Hello(op 10) → Identify(op 2) 或 Resume(op 6) → Dispatch(op 0: READY / RESUMED / 消息);
// 心跳 op 1 / ACK op 11;服务端要求重连 op 7;会话失效 op 9(d=true 可恢复)。

#include "im/qqbot/qq_protocol.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace acecode::im::qqbot {

class GatewaySession {
public:
    using Clock = std::chrono::steady_clock;

    struct Output {
        std::vector<std::string> frames;  // 要发送的 JSON 文本帧
        std::vector<std::pair<std::string, nlohmann::json>> dispatches;  // (事件类型, d)
        bool ready = false;      // 本帧让连接可用(READY / RESUMED)
        bool reconnect = false;  // 服务端要求重连(op 7 / op 9)
        std::string bot_id;      // READY 里的机器人 id(可空)
        std::string bot_name;    // READY 里的机器人名称(可空)
    };

    struct Tick {
        std::optional<std::string> heartbeat;  // 到期要发的心跳帧
        bool zombie = false;  // 上一次心跳一直没等到 ACK:连接已假死,应断开重连
    };

    // WebSocket 握手成功后调用;token 为不带前缀的访问令牌。
    void on_connected(std::string token, Clock::time_point now);
    Output on_frame(const std::string& text, Clock::time_point now);
    Tick on_tick(Clock::time_point now);
    // 连接关闭后调用,按关闭码决定下一次是恢复还是重新登录。
    CloseDecision on_closed(int code);
    // 平台明确要求重新登录(或凭据变化)时清掉会话。
    void clear_session();

    bool can_resume() const { return !session_id_.empty() && seq_.has_value(); }
    const std::string& session_id() const { return session_id_; }
    std::optional<std::int64_t> last_seq() const { return seq_; }
    std::chrono::milliseconds heartbeat_interval() const { return interval_; }
    bool ready() const { return ready_; }

private:
    std::string heartbeat_frame() const;

    std::string token_;
    std::string session_id_;
    std::optional<std::int64_t> seq_;
    std::chrono::milliseconds interval_{std::chrono::seconds(30)};
    Clock::time_point next_beat_{};
    bool hello_ = false;
    bool awaiting_ack_ = false;
    bool ready_ = false;
};

} // namespace acecode::im::qqbot
