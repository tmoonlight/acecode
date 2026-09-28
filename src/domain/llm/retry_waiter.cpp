#include "retry_waiter.hpp"

#include <limits>

namespace acecode {

bool ProviderRetryWaiter::wait_for(
    std::chrono::milliseconds delay,
    const std::atomic<bool>* abort_flag) {
    if (abort_flag && abort_flag->load()) return true;
    if (delay.count() <= 0) return false;

    std::unique_lock<std::mutex> lock(mu_);
    const std::uint64_t observed_generation = wake_generation_;
    cv_.wait_for(lock, delay, [&]() {
        return wake_generation_ != observed_generation ||
               (abort_flag && abort_flag->load());
    });
    return abort_flag && abort_flag->load();
}

void ProviderRetryWaiter::wake() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (wake_generation_ !=
            (std::numeric_limits<std::uint64_t>::max)()) {
            ++wake_generation_;
        } else {
            wake_generation_ = 0;
        }
    }
    cv_.notify_all();
}

void ProviderRetryWaiter::notify_cancelled_request() {
    // Pair with wait_for's mutex so cancellation cannot be lost between its
    // predicate check and entering the wait. Unlike wake(), no generation is
    // changed: requests whose own abort flag remains false keep waiting.
    {
        std::lock_guard<std::mutex> lock(mu_);
    }
    cv_.notify_all();
}

} // namespace acecode
