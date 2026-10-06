#pragma once

// 钉钉出站路由(纯逻辑,不做 IO):
//   - Route:一次回复需要的平台信息(会话 webhook 及其到期时间、conversationId、staffId、robotCode);
//   - RouteBook:按会话键记住最近一条入站消息带来的 Route(上限 500 条,超出淘汰最久未更新的),
//     并记住已被平台判定失效的 webhook,避免从持久化的回复上下文里再次拿出来用;
//   - RateLimiter:同一会话每 60 秒最多 20 条(钉钉机器人发送频率上限,超出会被封 10 分钟),
//     平台报“发送过快”时按要求暂停该会话。调用方先 reserve 得到发送时刻,等到该时刻再发。

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

namespace acecode::im::dingtalk {

struct Route {
    std::string webhook;
    std::int64_t webhook_expires_ms = 0;  // 墙钟毫秒(sessionWebhookExpiredTime)
    std::string conversation_id;
    bool group = false;
    std::string staff_id;
    std::string robot_code;
    std::string msg_id;
};

// 从入站消息的 reply_context 还原 Route;字段缺失时保持空值。
Route route_from_context(const nlohmann::json& reply_context);

// webhook 在 now_ms + margin_ms 之后仍然有效(提前 margin 视为过期,Hermes 用 5 分钟)。
bool webhook_usable(const Route& route, std::int64_t now_ms, std::int64_t margin_ms);

class RouteBook {
public:
    explicit RouteBook(std::size_t capacity = 500, std::size_t dead_capacity = 500);

    // 记录某会话最新一条入站消息的 Route(覆盖旧值并刷新淘汰顺序)。
    void remember(const std::string& key, const Route& route);
    std::optional<Route> find(const std::string& key) const;

    // 合并缓存与本次的回复上下文:webhook 取两者中仍可用且到期更晚的一个(已失效的跳过),
    // staffId / robotCode / conversationId 缺失时互相补齐。
    Route resolve(const std::string& key, const nlohmann::json& reply_context, std::int64_t now_ms,
                  std::int64_t margin_ms) const;

    // 平台说这个 webhook 已失效(session 不存在):以后不再使用。
    void mark_webhook_dead(const std::string& webhook);
    bool webhook_dead(const std::string& webhook) const;
    std::size_t size() const;

private:
    struct Entry {
        Route route;
        std::list<std::string>::iterator order;
    };

    std::size_t capacity_;
    std::size_t dead_capacity_;
    mutable std::mutex mu_;
    std::list<std::string> order_;  // 最近更新的在尾部
    std::unordered_map<std::string, Entry> entries_;
    std::set<std::string> dead_;
    std::deque<std::string> dead_order_;
};

class RateLimiter {
public:
    using Clock = std::chrono::steady_clock;

    struct Limits {
        std::size_t per_window = 20;
        std::chrono::milliseconds window{std::chrono::minutes(1)};
    };

    RateLimiter() = default;
    explicit RateLimiter(Limits limits) : limits_(limits) {}

    // 预约一条消息的发送时刻;返回值不早于 now。预约即占用名额。
    Clock::time_point reserve(const std::string& chat, Clock::time_point now);
    // 平台对该会话报“发送过快”:wait 之内不再预约到该会话。
    void penalize(const std::string& chat, std::chrono::milliseconds wait, Clock::time_point now);

private:
    Limits limits_;
    std::mutex mu_;
    std::map<std::string, Clock::time_point> blocked_until_;
    std::map<std::string, std::deque<Clock::time_point>> sends_;
};

// 有界去重集合:按插入顺序淘汰,超过 capacity 时丢最旧的。线程安全。
class DedupSet {
public:
    explicit DedupSet(std::size_t capacity = 2000) : capacity_(capacity) {}
    // 第一次见到返回 false 并记下;见过返回 true。
    bool seen(const std::string& key);
    bool contains(const std::string& key) const;

private:
    std::size_t capacity_;
    mutable std::mutex mu_;
    std::set<std::string> keys_;
    std::deque<std::string> order_;
};

} // namespace acecode::im::dingtalk
