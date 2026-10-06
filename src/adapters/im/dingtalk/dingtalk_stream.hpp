#pragma once

// 钉钉 Stream 长连接的帧编解码与会话状态(纯逻辑,不做 IO)。
//
// 下行帧:{"specVersion","type":"SYSTEM|EVENT|CALLBACK","headers":{topic,messageId,…},"data":"<JSON 字符串>"}。
// 上行回执:{"code":200,"headers":{"contentType":"application/json","messageId":<原帧>},"message":"OK","data":"<字符串>"}。
// 规则(官方 SDK 一致):
//   - 机器人消息(CALLBACK /v1.0/im/bot/messages/get)先回执、后处理;不回执或回非 200 会被重投;
//   - SYSTEM ping 必须回执,data 原样回显;
//   - SYSTEM disconnect 先回执,再关连接并立即用新 ticket 重连(先关再回执会让平台重发断开指令);
//   - 未订阅的 CALLBACK 主题回 404;EVENT(未订阅,仍到达时)回 SUCCESS 防止重投;
//   - 头部字段的值可能是字符串也可能是数字,一律按“字符串或数字”读取。

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace acecode::im::dingtalk {

struct StreamFrame {
    std::string type;        // 大写:SYSTEM / EVENT / CALLBACK
    std::string topic;
    std::string message_id;  // headers.messageId;disconnect 帧可能没有
    nlohmann::json headers = nlohmann::json::object();
    std::string data;        // data 原文(本身是 JSON 字符串);缺失时为空
};

// 解析下行帧;不是 JSON 对象或缺 type 时返回 nullopt。
std::optional<StreamFrame> parse_stream_frame(const std::string& text);

// 构造上行回执帧。data 是要放进 "data" 字段的字符串(本身通常是 JSON 文本)。
std::string stream_reply(const std::string& message_id, int code, const std::string& message,
                         const std::string& data);

// 机器人消息回执的 data(协议文档:该主题不消费回执内容)。
inline constexpr const char* kCallbackAckData = "{\"response\":null}";
inline constexpr const char* kEventAckData = "{\"status\":\"SUCCESS\",\"message\":\"success\"}";

class StreamSession {
public:
    using Clock = std::chrono::steady_clock;

    struct Message {
        std::string message_id;  // 帧头 messageId(每次重投都不同)
        std::string data;        // 机器人消息 JSON 文本(待二次解析)
    };

    struct Output {
        std::vector<std::string> replies;  // 要立刻按顺序发出的回执帧
        std::vector<Message> messages;     // 已回执的机器人消息
        bool disconnect = false;           // 平台要求断开:发完回执后关闭并立即重连
        bool deferred = false;             // 本地队列已满,机器人消息未回执(平台稍后重投)
        bool invalid = false;              // 无法解析的帧(只记日志)
        std::string system_topic;          // 收到的 SYSTEM 帧主题(日志用)
        std::string ignored_topic;         // 未订阅的 CALLBACK / EVENT 主题(已回执,日志用)
    };

    // idle_timeout:连续这么久收不到任何帧就判定连接已失效;0 表示不检测。
    explicit StreamSession(std::chrono::milliseconds idle_timeout = std::chrono::seconds(120));

    // WebSocket 握手成功后调用。
    void on_connected(Clock::time_point now);
    // accept_messages=false 时机器人消息既不回执也不派发,让平台稍后重投(本地处理队列已满)。
    Output on_frame(const std::string& text, Clock::time_point now, bool accept_messages = true);
    // 到期检测:超过 idle_timeout 没收到任何帧时返回 true。
    bool idle_expired(Clock::time_point now) const;

    std::size_t frames() const { return frames_; }
    Clock::time_point connected_at() const { return connected_at_; }

private:
    std::chrono::milliseconds idle_timeout_;
    Clock::time_point connected_at_{};
    Clock::time_point last_frame_{};
    std::size_t frames_ = 0;
};

// 重连等待:min(base·2^attempt, cap),jitter 在 [0, jitter) 内由调用方给出。
std::chrono::milliseconds stream_backoff(std::size_t attempt, std::chrono::milliseconds base,
                                         std::chrono::milliseconds cap, std::chrono::milliseconds jitter);

} // namespace acecode::im::dingtalk
