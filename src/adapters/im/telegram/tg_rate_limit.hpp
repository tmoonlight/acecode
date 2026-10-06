#pragma once

// Telegram 发送限速(纯逻辑,design D11):同一聊天相邻两条至少间隔 1 秒;
// 同一个群每 60 秒最多 20 条;平台返回 429 时按 retry_after 暂停该聊天。
// 调用方先 reserve 得到发送时刻,睡到该时刻再发。

#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>

namespace acecode::im::telegram {

class RateLimiter {
public:
    using Clock = std::chrono::steady_clock;

    struct Limits {
        std::chrono::milliseconds per_chat_gap{std::chrono::seconds(1)};
        std::size_t group_per_minute = 20;
        std::chrono::milliseconds group_window{std::chrono::minutes(1)};
    };

    RateLimiter() = default;
    explicit RateLimiter(Limits limits) : limits_(limits) {}

    // 预约一条消息的发送时刻;返回值不早于 now。预约即占用名额。
    Clock::time_point reserve(const std::string& chat, bool group, Clock::time_point now);
    // 平台对该聊天返回 429:retry_after 之内不再预约到该聊天。
    void penalize(const std::string& chat, std::chrono::seconds retry_after, Clock::time_point now);

private:
    Limits limits_;
    std::mutex mu_;
    std::map<std::string, Clock::time_point> next_allowed_;
    std::map<std::string, std::deque<Clock::time_point>> group_sends_;
};

} // namespace acecode::im::telegram
