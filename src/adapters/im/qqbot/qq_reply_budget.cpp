#include "qq_reply_budget.hpp"

namespace acecode::im::qqbot {
namespace {

constexpr std::int64_t kPruneAfterMs = 2LL * 60 * 60 * 1000;  // 两小时后两种窗口都早已过期
constexpr std::size_t kMaxEntries = 4096;

} // namespace

ReplyBudget::ReplyBudget(ReplyLimits limits) : limits_(limits) {}

std::int64_t ReplyBudget::next_seq() {
    std::lock_guard<std::mutex> lock(mu_);
    return ++seq_;
}

void ReplyBudget::prune_locked(std::int64_t now_ms) {
    if (entries_.size() < kMaxEntries / 2) return;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (now_ms - it->second.received_at_ms > kPruneAfterMs) it = entries_.erase(it);
        else ++it;
    }
    while (entries_.size() >= kMaxEntries) entries_.erase(entries_.begin());
}

ReplyPlan ReplyBudget::plan(const nlohmann::json& reply_context, std::int64_t now_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    ReplyPlan plan;
    plan.msg_seq = ++seq_;
    if (!reply_context.is_object()) return plan;
    const auto msg_id = reply_context.value("msg_id", std::string{});
    if (msg_id.empty()) return plan;
    const bool group = reply_context.value("scope", std::string{}) == "group";
    const auto received = reply_context.value("received_at_ms", now_ms);
    const auto window = group ? limits_.group_window : limits_.c2c_window;
    const int max = group ? limits_.group_max : limits_.c2c_max;
    prune_locked(now_ms);
    auto& entry = entries_[msg_id];
    entry.received_at_ms = received;
    if (entry.exhausted || entry.used >= max || now_ms - received >= window.count()) return plan;
    ++entry.used;
    plan.passive = true;
    plan.msg_id = msg_id;
    return plan;
}

void ReplyBudget::exhaust(const std::string& msg_id) {
    std::lock_guard<std::mutex> lock(mu_);
    entries_[msg_id].exhausted = true;
}

std::size_t HeldQueue::push(const std::string& conversation_key, HeldItem item) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& queue = items_[conversation_key];
    queue.push_back(std::move(item));
    std::size_t dropped = 0;
    while (queue.size() > kMaxPerConversation) {
        queue.pop_front();
        ++dropped;
    }
    return dropped;
}

std::deque<HeldItem> HeldQueue::take(const std::string& conversation_key) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = items_.find(conversation_key);
    if (it == items_.end()) return {};
    auto queue = std::move(it->second);
    items_.erase(it);
    return queue;
}

std::size_t HeldQueue::size(const std::string& conversation_key) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = items_.find(conversation_key);
    return it == items_.end() ? 0 : it->second.size();
}

std::size_t HeldQueue::total() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t sum = 0;
    for (const auto& item : items_) sum += item.second.size();
    return sum;
}

} // namespace acecode::im::qqbot
