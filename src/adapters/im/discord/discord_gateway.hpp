#pragma once

// Discord 网关会话状态机与登录(Identify)预算,纯逻辑,不做 IO。
//
// 协议:连接 → Hello(op 10)→ Identify(op 2)或 Resume(op 6)→ Dispatch(op 0:READY / RESUMED / 事件)。
// 心跳:首次在 interval × jitter(jitter ∈ [0,1))后发送,之后每 interval 一次;下一次到期时上一拍
// 还没收到 ACK(op 11)即判定连接假死,应以非 1000 关闭码断开并恢复会话。服务端发 op 1 时立即补一拍。
// op 7:断开并恢复;op 9:d=true 可恢复,d=false 会话作废,等 1–5 秒后重新 Identify。
//
// 登录预算:Discord 每个机器人 24 小时最多 1000 次 Identify(Resume 不算),超出后会重置机器人
// token。所以能 Resume 就 Resume,Identify 之间至少间隔 5 秒,连续失败指数退避(5s→10s→20s…
// 封顶 5 分钟),24 小时内超过 900 次就暂停到窗口结束。

#include "im/discord/discord_protocol.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace acecode::im::discord {

enum class CloseAction {
    Resume,       // 普通断线:带 session_id + seq 恢复
    Identify,     // 会话已失效:重新登录
    RateLimited,  // 网关频控(4008):至少等 60 秒再恢复
    Fatal,        // token 无效、intent 未开等:停止重连,等用户处理
};

struct CloseDecision {
    CloseAction action = CloseAction::Resume;
    std::string reason;  // 给用户看的中文原因
};

CloseDecision classify_close(int code);

struct ReadyInfo {
    std::string bot_id;
    std::string bot_name;
    std::string application_id;
    std::optional<std::int64_t> application_flags;
    std::size_t guild_count = 0;
};

class GatewaySession {
public:
    using Clock = std::chrono::steady_clock;

    struct Output {
        std::vector<std::string> frames;  // 要发送的 JSON 文本帧
        std::vector<std::pair<std::string, nlohmann::json>> dispatches;  // (事件类型, d),不含 READY/RESUMED
        bool ready = false;            // 本帧让连接可用(READY 或 RESUMED)
        bool resumed = false;          // 是 RESUMED
        bool identified = false;       // 本帧发出了 Identify(调用方要记入登录预算)
        bool reconnect = false;        // 需要断开重连(op 7 / op 9)
        bool invalid_session = false;  // op 9 d=false:会话作废,以 1000 关闭,随机等 1–5 秒后重新 Identify
        std::optional<ReadyInfo> ready_info;
    };

    struct Tick {
        std::optional<std::string> heartbeat;  // 到期要发的心跳帧
        bool zombie = false;         // 上一拍没等到 ACK:连接假死
        bool hello_timeout = false;  // 连上后迟迟没收到 Hello
    };

    explicit GatewaySession(int intents = kIntents) : intents_(intents) {}

    // WebSocket 握手成功后调用。token 为原始 bot token(不带 "Bot " 前缀);jitter ∈ [0,1)
    // 决定首拍心跳的时刻。
    void on_connected(std::string token, Clock::time_point now, double jitter);
    Output on_frame(const std::string& text, Clock::time_point now);
    Tick on_tick(Clock::time_point now);
    // 连接关闭后调用,按关闭码决定下一次是恢复、重新登录还是停止。
    CloseDecision on_closed(int code);
    void clear_session();

    void set_hello_timeout(std::chrono::milliseconds timeout) { hello_timeout_ = timeout; }

    bool can_resume() const { return !session_id_.empty() && seq_.has_value(); }
    const std::string& session_id() const { return session_id_; }
    // READY 给出的恢复地址(基址,不含查询串);可能为空。
    const std::string& resume_url() const { return resume_url_; }
    std::optional<std::int64_t> last_seq() const { return seq_; }
    std::chrono::milliseconds heartbeat_interval() const { return interval_; }
    bool ready() const { return ready_; }
    bool hello_received() const { return hello_; }

    std::string identify_frame() const;
    std::string resume_frame() const;
    std::string heartbeat_frame() const;

private:
    int intents_;
    std::string token_;
    std::string session_id_;
    std::string resume_url_;
    std::optional<std::int64_t> seq_;
    std::chrono::milliseconds interval_{std::chrono::seconds(41)};
    std::chrono::milliseconds hello_timeout_{std::chrono::seconds(20)};
    double jitter_ = 0.0;
    Clock::time_point next_beat_{};
    Clock::time_point hello_deadline_{};
    bool connected_ = false;
    bool hello_ = false;
    bool awaiting_ack_ = false;
    bool ready_ = false;
};

// 登录预算(纯逻辑)。时间一律用 Unix 毫秒,便于持久化 24 小时计数。
class IdentifyBudget {
public:
    struct Limits {
        std::chrono::milliseconds min_interval{std::chrono::seconds(5)};
        std::chrono::milliseconds max_backoff{std::chrono::minutes(5)};
        std::int64_t daily_cap = 900;  // Discord 硬上限 1000,留 100 次余量
        std::chrono::milliseconds window{std::chrono::hours(24)};
    };

    IdentifyBudget() = default;
    // window_start_ms / count 来自持久化(0 表示还没有记录)。
    IdentifyBudget(Limits limits, std::int64_t window_start_ms, std::int64_t count);

    // 最早允许 Identify 的时刻;<= now_ms 表示现在就可以。
    std::int64_t next_allowed_ms(std::int64_t now_ms) const;
    // 当前 24 小时窗口内已达上限。
    bool daily_cap_reached(std::int64_t now_ms) const;
    void record_identify(std::int64_t now_ms);
    // Identify 之后收到 READY:清零连续失败计数。
    void record_ready();
    // Identify 之后没等到 READY 就断开 / op 9:连续失败计数 +1,下一次按指数退避。
    void record_failure();
    // GET /gateway/bot 的 session_start_limit.remaining == 0:在 until_ms 之前不登录。
    void block_until(std::int64_t until_ms);

    std::int64_t window_start_ms() const { return window_start_; }
    std::int64_t count() const { return count_; }
    int failures() const { return failures_; }

private:
    bool window_active(std::int64_t now_ms) const;

    Limits limits_;
    std::int64_t window_start_ = 0;
    std::int64_t count_ = 0;
    std::optional<std::int64_t> last_identify_;
    int failures_ = 0;
    std::int64_t blocked_until_ = 0;
};

} // namespace acecode::im::discord
