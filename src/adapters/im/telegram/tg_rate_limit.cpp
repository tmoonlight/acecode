#include "tg_rate_limit.hpp"

namespace acecode::im::telegram {

RateLimiter::Clock::time_point RateLimiter::reserve(const std::string& chat, bool group, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    auto at = now;
    const auto allowed = next_allowed_.find(chat);
    if (allowed != next_allowed_.end() && allowed->second > at) at = allowed->second;
    if (group) {
        auto& sends = group_sends_[chat];
        while (true) {
            while (!sends.empty() && sends.front() + limits_.group_window <= at) sends.pop_front();
            if (sends.size() < limits_.group_per_minute) break;
            at = sends.front() + limits_.group_window;
        }
        sends.push_back(at);
    }
    next_allowed_[chat] = at + limits_.per_chat_gap;
    return at;
}

void RateLimiter::penalize(const std::string& chat, std::chrono::seconds retry_after, Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& allowed = next_allowed_[chat];
    const auto until = now + retry_after;
    if (allowed < until) allowed = until;
}

} // namespace acecode::im::telegram
