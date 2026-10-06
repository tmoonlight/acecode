#pragma once

// 飞书发送节流(纯逻辑):同一接收方(群或用户)每秒最多 5 条,群内由所有机器人共享;
// 这里按每个接收方相邻两条至少间隔 gap(默认 250 ms,即 ≤ 4 条/秒)预约发送时刻。
// 被平台限流时 penalize,该接收方在等待期内不再预约。调用方先 reserve,睡到返回的时刻再发。

#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace acecode::im::feishu {

class SendPacer {
public:
    using Clock = std::chrono::steady_clock;

    explicit SendPacer(std::chrono::milliseconds gap = std::chrono::milliseconds(250)) : gap_(gap) {}

    // 预约一条消息的发送时刻;返回值不早于 now。预约即占用名额。
    Clock::time_point reserve(const std::string& target, Clock::time_point now);
    // 平台对该接收方限流:wait 之内不再预约到它。
    void penalize(const std::string& target, std::chrono::milliseconds wait, Clock::time_point now);
    std::size_t tracked() const;

private:
    void prune_locked(Clock::time_point now);

    std::chrono::milliseconds gap_;
    mutable std::mutex mu_;
    std::map<std::string, Clock::time_point> next_allowed_;
};

} // namespace acecode::im::feishu
