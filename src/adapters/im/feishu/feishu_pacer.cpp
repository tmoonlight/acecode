#include "im/feishu/feishu_pacer.hpp"

namespace acecode::im::feishu {
namespace {

// 记录太多接收方时清掉早已过期的条目,避免长期运行时无限增长。
constexpr std::size_t kPruneThreshold = 1024;

} // namespace

void SendPacer::prune_locked(Clock::time_point now) {
    if (next_allowed_.size() < kPruneThreshold) return;
    for (auto it = next_allowed_.begin(); it != next_allowed_.end();) {
        if (it->second <= now) it = next_allowed_.erase(it);
        else ++it;
    }
}

SendPacer::Clock::time_point SendPacer::reserve(const std::string& target, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    prune_locked(now);
    auto at = now;
    const auto found = next_allowed_.find(target);
    if (found != next_allowed_.end() && found->second > at) at = found->second;
    next_allowed_[target] = at + gap_;
    return at;
}

void SendPacer::penalize(const std::string& target, std::chrono::milliseconds wait, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& allowed = next_allowed_[target];
    const auto until = now + wait;
    if (allowed < until) allowed = until;
}

std::size_t SendPacer::tracked() const {
    std::lock_guard<std::mutex> lock(mu_);
    return next_allowed_.size();
}

} // namespace acecode::im::feishu
