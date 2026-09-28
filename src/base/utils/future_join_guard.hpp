#pragma once

#include <cstddef>
#include <future>
#include <utility>
#include <vector>

namespace acecode::utils {

// Owns asynchronous operations until every one has completed, including when
// result delivery throws. get() preserves the caller's consumption order.
template<class T>
class FutureJoinGuard {
public:
    FutureJoinGuard() = default;
    FutureJoinGuard(const FutureJoinGuard&) = delete;
    FutureJoinGuard& operator=(const FutureJoinGuard&) = delete;
    ~FutureJoinGuard() { join(); }

    std::size_t add(std::future<T> future) {
        futures_.push_back(std::move(future));
        return futures_.size() - 1;
    }

    T get(std::size_t index) { return futures_.at(index).get(); }

    void join() noexcept {
        for (auto& future : futures_) {
            if (!future.valid()) continue;
            try { future.wait(); } catch (...) {
                // A failed wait must not prevent joining the remaining calls.
                // An explicit get() remains the result/error delivery point.
            }
        }
    }

private:
    std::vector<std::future<T>> futures_;
};

} // namespace acecode::utils
