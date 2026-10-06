#include "dingtalk_route.hpp"

#include <iterator>

namespace acecode::im::dingtalk {
namespace {

std::string text(const nlohmann::json& value, const char* key) {
    if (!value.is_object()) return {};
    const auto it = value.find(key);
    if (it == value.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<std::int64_t>());
    return {};
}

std::int64_t number(const nlohmann::json& value, const char* key) {
    if (!value.is_object()) return 0;
    const auto it = value.find(key);
    if (it == value.end()) return 0;
    if (it->is_number_integer()) return it->get<std::int64_t>();
    if (it->is_number_unsigned()) return static_cast<std::int64_t>(it->get<std::uint64_t>());
    if (it->is_string()) {
        try {
            return std::stoll(it->get<std::string>());
        } catch (...) {
        }
    }
    return 0;
}

} // namespace

Route route_from_context(const nlohmann::json& reply_context) {
    Route route;
    if (!reply_context.is_object()) return route;
    route.webhook = text(reply_context, "sessionWebhook");
    route.webhook_expires_ms = number(reply_context, "sessionWebhookExpiredTime");
    route.conversation_id = text(reply_context, "conversationId");
    route.group = text(reply_context, "conversationType") == "2";
    route.staff_id = text(reply_context, "senderStaffId");
    route.robot_code = text(reply_context, "robotCode");
    route.msg_id = text(reply_context, "msgId");
    return route;
}

bool webhook_usable(const Route& route, std::int64_t now_ms, std::int64_t margin_ms) {
    return !route.webhook.empty() && route.webhook_expires_ms > 0 && now_ms + margin_ms < route.webhook_expires_ms;
}

RouteBook::RouteBook(std::size_t capacity, std::size_t dead_capacity)
    : capacity_(capacity == 0 ? 1 : capacity), dead_capacity_(dead_capacity == 0 ? 1 : dead_capacity) {}

void RouteBook::remember(const std::string& key, const Route& route) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = entries_.find(key);
    if (it != entries_.end()) {
        order_.erase(it->second.order);
        entries_.erase(it);
    }
    order_.push_back(key);
    entries_[key] = Entry{route, std::prev(order_.end())};
    while (entries_.size() > capacity_) {
        entries_.erase(order_.front());
        order_.pop_front();
    }
}

std::optional<Route> RouteBook::find(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) return std::nullopt;
    return it->second.route;
}

Route RouteBook::resolve(const std::string& key, const nlohmann::json& reply_context, std::int64_t now_ms,
                         std::int64_t margin_ms) const {
    auto route = route_from_context(reply_context);
    std::lock_guard<std::mutex> lock(mu_);
    const bool context_ok = webhook_usable(route, now_ms, margin_ms) && !dead_.count(route.webhook);
    if (!context_ok) {
        route.webhook.clear();
        route.webhook_expires_ms = 0;
    }
    const auto it = entries_.find(key);
    if (it == entries_.end()) return route;
    const auto& cached = it->second.route;
    const bool cached_ok = webhook_usable(cached, now_ms, margin_ms) && !dead_.count(cached.webhook);
    if (cached_ok && (!context_ok || cached.webhook_expires_ms > route.webhook_expires_ms)) {
        route.webhook = cached.webhook;
        route.webhook_expires_ms = cached.webhook_expires_ms;
    }
    if (route.staff_id.empty()) route.staff_id = cached.staff_id;
    if (route.robot_code.empty()) route.robot_code = cached.robot_code;
    if (route.conversation_id.empty()) route.conversation_id = cached.conversation_id;
    if (route.msg_id.empty()) route.msg_id = cached.msg_id;
    if (!reply_context.is_object() || !reply_context.contains("conversationType")) route.group = cached.group;
    return route;
}

void RouteBook::mark_webhook_dead(const std::string& webhook) {
    if (webhook.empty()) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (!dead_.insert(webhook).second) return;
    dead_order_.push_back(webhook);
    while (dead_order_.size() > dead_capacity_) {
        dead_.erase(dead_order_.front());
        dead_order_.pop_front();
    }
}

bool RouteBook::webhook_dead(const std::string& webhook) const {
    std::lock_guard<std::mutex> lock(mu_);
    return dead_.count(webhook) > 0;
}

std::size_t RouteBook::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.size();
}

RateLimiter::Clock::time_point RateLimiter::reserve(const std::string& chat, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    auto at = now;
    const auto blocked = blocked_until_.find(chat);
    if (blocked != blocked_until_.end() && blocked->second > at) at = blocked->second;
    auto& sends = sends_[chat];
    const auto per_window = limits_.per_window == 0 ? std::size_t{1} : limits_.per_window;
    while (true) {
        while (!sends.empty() && sends.front() + limits_.window <= at) sends.pop_front();
        if (sends.size() < per_window) break;
        at = sends.front() + limits_.window;
    }
    sends.push_back(at);
    return at;
}

void RateLimiter::penalize(const std::string& chat, std::chrono::milliseconds wait, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& until = blocked_until_[chat];
    const auto candidate = now + wait;
    if (until < candidate) until = candidate;
}

bool DedupSet::seen(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    if (keys_.count(key)) return true;
    keys_.insert(key);
    order_.push_back(key);
    while (order_.size() > capacity_) {
        keys_.erase(order_.front());
        order_.pop_front();
    }
    return false;
}

bool DedupSet::contains(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mu_);
    return keys_.count(key) > 0;
}

} // namespace acecode::im::dingtalk
