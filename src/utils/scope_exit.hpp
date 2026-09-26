#pragma once

#include <type_traits>
#include <utility>

namespace acecode {

// A function-local cleanup. Invoking or destroying the callable must not throw.
template <typename Fn>
class ScopeExit {
public:
    explicit ScopeExit(Fn fn) noexcept(std::is_nothrow_move_constructible_v<Fn>)
        : fn_(std::move(fn)) {}

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit& operator=(ScopeExit&&) = delete;

    ScopeExit(ScopeExit&& other) noexcept(std::is_nothrow_move_constructible_v<Fn>)
        : fn_(std::move(other.fn_)), active_(std::exchange(other.active_, false)) {}

    ~ScopeExit() noexcept {
        if (active_) fn_();
    }

    void release() noexcept { active_ = false; }

private:
    Fn fn_;
    bool active_ = true;
};

template <typename Fn>
ScopeExit(Fn) -> ScopeExit<Fn>;

}  // namespace acecode
