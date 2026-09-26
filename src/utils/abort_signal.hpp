#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace acecode {

// The mutex is a leaf: no application code runs while it is held.
class AbortSignal {
public:
    void request() noexcept {
        {
            std::lock_guard<std::mutex> lock(mu_);
            requested_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
    }

    // Clear only at the start of a new operation, after the previous waiters end.
    void clear() noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        requested_.store(false, std::memory_order_release);
    }

    template <typename Rep, typename Period>
    bool wait_for(std::chrono::duration<Rep, Period> timeout) const {
        std::unique_lock<std::mutex> lock(mu_);
        // The borrowed predicate runs synchronously inside wait_for only.
        return cv_.wait_for(lock, timeout, [requested = &requested_] {
            return requested->load(std::memory_order_acquire);
        });
    }

    const std::atomic<bool>& raw() const noexcept { return requested_; }

    // Borrowed, for synchronous legacy APIs only. Direct stores do not notify
    // waiters; use request() for cancellation that needs to wake wait_for().
    std::atomic<bool>& flag_for_legacy_api() noexcept { return requested_; }

private:
    std::atomic<bool> requested_{false};
    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
};

}  // namespace acecode
