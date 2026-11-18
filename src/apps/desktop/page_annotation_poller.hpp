#pragma once

#include <functional>
#include <memory>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#include <unordered_map>
#elif defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

namespace acecode::desktop {

class AnnotationPoller {
    struct State { std::mutex mutex; bool active = true; std::function<void()> tick; };
    std::shared_ptr<State> state_;
    static bool invoke(const std::shared_ptr<State>& state) {
        std::function<void()> tick;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->active) return false;
            tick = state->tick;
        }
        if (tick) tick();
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->active;
    }
#ifdef _WIN32
    UINT_PTR timer_ = 0;
    inline static std::mutex timers_mutex_;
    static auto& timers() {
        static std::unordered_map<UINT_PTR, std::weak_ptr<State>> entries;
        return entries;
    }
#elif defined(__APPLE__)
    static void schedule(const std::shared_ptr<State>& state) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            if (invoke(state)) schedule(state);
        });
    }
#endif
public:
    AnnotationPoller() = default;
    AnnotationPoller(const AnnotationPoller&) = delete;
    AnnotationPoller& operator=(const AnnotationPoller&) = delete;
    ~AnnotationPoller() { stop(); }
    void stop() {
        std::function<void()> retired;
        if (state_) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->active = false;
            retired = std::move(state_->tick);
        }
#ifdef _WIN32
        if (timer_) {
            KillTimer(nullptr, timer_);
            std::lock_guard<std::mutex> lock(timers_mutex_);
            timers().erase(timer_);
            timer_ = 0;
        }
#endif
        state_.reset();
    }
    void start(std::function<void()> tick) {
        if (state_) return;
        state_ = std::make_shared<State>();
        state_->tick = std::move(tick);
#ifdef _WIN32
        timer_ = SetTimer(nullptr, 0, 250, [](HWND, UINT, UINT_PTR timer, DWORD) {
            std::shared_ptr<State> state;
            {
                std::lock_guard<std::mutex> lock(timers_mutex_);
                const auto found = timers().find(timer);
                if (found != timers().end()) state = found->second.lock();
            }
            if (state) invoke(state);
        });
        if (timer_) {
            std::lock_guard<std::mutex> lock(timers_mutex_);
            timers()[timer_] = state_;
        } else stop();
#elif defined(__APPLE__)
        schedule(state_);
#endif
    }
};

}
