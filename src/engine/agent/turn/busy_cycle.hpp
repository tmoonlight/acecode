#pragma once

#include <exception>
#include <utility>

namespace acecode::agent {

// Function-local, borrowed terminal operation. Worker recovery owns terminal
// reporting during exception unwinding; it must never be emitted twice.
template <typename Finish>
class BusyCycleScope {
public:
    explicit BusyCycleScope(Finish finish)
        : finish_(std::move(finish)), exceptions_(std::uncaught_exceptions()) {}
    BusyCycleScope(const BusyCycleScope&) = delete;
    BusyCycleScope& operator=(const BusyCycleScope&) = delete;
    ~BusyCycleScope() noexcept(false) {
        if (std::uncaught_exceptions() == exceptions_) finish();
    }
    void finish() {
        if (!active_) return;
        active_ = false;
        finish_();
    }
    void operator()() { finish(); }

private:
    Finish finish_;
    int exceptions_;
    bool active_ = true;
};

template <typename Finish>
BusyCycleScope(Finish) -> BusyCycleScope<Finish>;

} // namespace acecode::agent
