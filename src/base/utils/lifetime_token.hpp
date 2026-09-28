#pragma once

#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

namespace acecode {

namespace lifetime_detail {

struct State {
    std::mutex mu;
    std::condition_variable cv;
    bool alive = true;
    std::size_t in_flight = 0;
};

// Stack-linked, per-thread bookkeeping also detects nested callbacks. It does
// not allocate and never runs business code with the state leaf mutex held.
class Invocation {
public:
    explicit Invocation(std::shared_ptr<State> state) noexcept
        : state_(std::move(state)), previous_(current_) { current_ = this; }

    Invocation(const Invocation&) = delete;
    Invocation& operator=(const Invocation&) = delete;

    ~Invocation() {
        current_ = previous_;
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            --state_->in_flight;
        }
        state_->cv.notify_all();
    }

    static bool contains(const State* state) noexcept {
        for (auto* invocation = current_; invocation; invocation = invocation->previous_) {
            if (invocation->state_.get() == state) return true;
        }
        return false;
    }

private:
    // Shared with the token; admitted callbacks must survive concurrent revoke.
    std::shared_ptr<State> state_;
    Invocation* previous_;  // Borrowed from this thread's active callback stack.
    inline static thread_local Invocation* current_ = nullptr;
};

}  // namespace lifetime_detail

template <typename T>
class LifetimeRef;

// Declare after the protected members. If the owner's destructor body accesses
// them, revoke at its very start as well. Moving a token transfers its revocation
// duty; it does not rebind existing refs to a different owner's address.
// Never revoke while holding an application lock an admitted callback needs.
class LifetimeToken {
public:
    LifetimeToken() : state_(std::make_shared<lifetime_detail::State>()) {}
    LifetimeToken(const LifetimeToken&) = delete;
    LifetimeToken& operator=(const LifetimeToken&) = delete;
    LifetimeToken(LifetimeToken&&) noexcept = default;

    LifetimeToken& operator=(LifetimeToken&& other) noexcept {
        if (this != &other) {
            revoke();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    ~LifetimeToken() { revoke(); }

    template <typename T>
    LifetimeRef<T> ref(T& owner) const noexcept;

    void revoke() noexcept {
        if (!state_) return;
        const bool inside_callback = lifetime_detail::Invocation::contains(state_.get());
        assert(!inside_callback && "LifetimeToken::revoke inside its own callback");
        // This is a programming error in release builds as well. Continuing
        // would either deadlock or let the callback outlive destroyed members.
        if (inside_callback) std::terminate();
        std::unique_lock<std::mutex> lock(state_->mu);
        state_->alive = false;
        state_->cv.wait(lock, [state = state_] { return state->in_flight == 0; });
    }

private:
    // Shared only with admitted invocations; refs themselves are weak.
    std::shared_ptr<lifetime_detail::State> state_;
};

template <typename T>
class LifetimeRef {
public:
    LifetimeRef() = default;

    // Returns whether fn ran. Neither owner nor a reference into it may escape
    // this invocation; callers that need results copy them inside fn.
    template <typename Fn>
    bool with(Fn&& fn) const {
        auto state = state_.lock();
        if (!state) return false;
        {
            std::lock_guard<std::mutex> lock(state->mu);
            if (!state->alive) return false;
            ++state->in_flight;
        }
        lifetime_detail::Invocation invocation(std::move(state));
        std::invoke(std::forward<Fn>(fn), *owner_);
        return true;
    }

private:
    friend class LifetimeToken;
    LifetimeRef(const std::shared_ptr<lifetime_detail::State>& state, T& owner) noexcept
        : state_(state), owner_(&owner) {}

    std::weak_ptr<lifetime_detail::State> state_;
    T* owner_ = nullptr;  // Nullable, borrowed; accessed only after admission.
};

template <typename T>
LifetimeRef<T> LifetimeToken::ref(T& owner) const noexcept {
    return LifetimeRef<T>(state_, owner);
}

}  // namespace acecode
