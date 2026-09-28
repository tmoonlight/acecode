#pragma once

#include "utils/scope_exit.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace acecode {

namespace thread_detail {

struct StopState {
    std::atomic<bool> requested{false};
    std::mutex mu;
    std::condition_variable cv;
};

// The only joining-thread detach exception: joining oneself would throw and
// leave a joinable std::thread to terminate the process. Use stderr, which does
// not depend on the application Logger's static lifetime.
inline void join_or_detach_self(std::thread& thread) {
    if (!thread.joinable()) return;
    if (thread.get_id() == std::this_thread::get_id()) {
        std::fputs("[joining_thread] self-join avoided; detaching owned thread\n", stderr);
        thread.detach();
    } else {
        thread.join();
    }
}

}  // namespace thread_detail

class JoiningThread;

class StopToken {
public:
    StopToken() = default;

    bool stop_requested() const noexcept {
        return state_ && state_->requested.load(std::memory_order_acquire);
    }

    template <typename Rep, typename Period>
    bool wait_for(std::chrono::duration<Rep, Period> timeout) const {
        if (!state_) return false;
        std::unique_lock<std::mutex> lock(state_->mu);
        return state_->cv.wait_for(lock, timeout, [state = state_] {
            return state->requested.load(std::memory_order_acquire);
        });
    }

private:
    friend class JoiningThread;
    explicit StopToken(std::shared_ptr<thread_detail::StopState> state)
        : state_(std::move(state)) {}

    // Shared by the thread owner and copied tokens, including a self-detached worker.
    std::shared_ptr<thread_detail::StopState> state_;
};

// C++17 jthread counterpart. Callable arguments are decay-copied, with an
// optional first StopToken argument. The callable must contain its exceptions.
class JoiningThread {
public:
    JoiningThread() noexcept = default;

    template <typename Fn, typename... Args,
              std::enable_if_t<!std::is_same_v<std::decay_t<Fn>, JoiningThread>, int> = 0>
    explicit JoiningThread(Fn&& fn, Args&&... args)
        : stop_(std::make_shared<thread_detail::StopState>()),
          thread_([token = StopToken(stop_), task = std::decay_t<Fn>(std::forward<Fn>(fn)),
                   values = std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...)]() mutable {
              std::apply([&task, token](auto&&... values) mutable {
                  if constexpr (std::is_invocable_v<std::decay_t<Fn>, StopToken,
                                                    std::decay_t<Args>...>) {
                      std::invoke(std::move(task), token, std::move(values)...);
                  } else {
                      std::invoke(std::move(task), std::move(values)...);
                  }
              }, std::move(values));
          }) {}

    JoiningThread(const JoiningThread&) = delete;
    JoiningThread& operator=(const JoiningThread&) = delete;

    JoiningThread(JoiningThread&& other) noexcept
        : stop_(std::move(other.stop_)), thread_(std::move(other.thread_)) {}

    JoiningThread& operator=(JoiningThread&& other) noexcept {
        if (this != &other) {
            request_stop();
            join();
            stop_ = std::move(other.stop_);
            thread_ = std::move(other.thread_);
        }
        return *this;
    }

    ~JoiningThread() {
        request_stop();
        join();
    }

    bool request_stop() noexcept {
        if (!stop_) return false;
        bool changed;
        {
            std::lock_guard<std::mutex> lock(stop_->mu);
            changed = !stop_->requested.exchange(true, std::memory_order_acq_rel);
        }
        stop_->cv.notify_all();
        return changed;
    }

    StopToken get_stop_token() const noexcept { return StopToken(stop_); }
    bool joinable() const noexcept { return thread_.joinable(); }
    std::thread::id get_id() const noexcept { return thread_.get_id(); }
    void join() { thread_detail::join_or_detach_self(thread_); }

private:
    // Shared with worker tokens, so stop state survives the self-join exception.
    std::shared_ptr<thread_detail::StopState> stop_;
    std::thread thread_;
};

// Lifted from daemon/worker.cpp. Its public thread collection and join order
// remain unchanged; the same self-join guard also applies here.
struct JoiningThreadGroup {
    ~JoiningThreadGroup() { join_all(); }

    void join_all() {
        for (auto& thread : threads) {
            thread_detail::join_or_detach_self(thread);
        }
        threads.clear();
    }

    std::vector<std::thread> threads;
};

// One thread per task, without retaining finished thread handles indefinitely.
// Reap/join never hold mu_: a task may itself enqueue more work into this set.
class ReapingThreadSet {
public:
    ReapingThreadSet() = default;
    ReapingThreadSet(const ReapingThreadSet&) = delete;
    ReapingThreadSet& operator=(const ReapingThreadSet&) = delete;

    ~ReapingThreadSet() { shutdown(); }

    template <typename Fn>
    bool spawn(Fn&& fn) {
        reap();
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_) return false;
        // Allocate the empty slot before creating the worker: allocation failure
        // must not join a just-started task under the collection mutex.
        entries_.push_back({done, {}});
        try {
            entries_.back().thread = JoiningThread(
                [done, task = std::decay_t<Fn>(std::forward<Fn>(fn))](StopToken stop) mutable {
                    ScopeExit finished([done] { done->store(true, std::memory_order_release); });
                    if constexpr (std::is_invocable_v<std::decay_t<Fn>, StopToken>) {
                        std::invoke(std::move(task), stop);
                    } else {
                        std::invoke(std::move(task));
                    }
                });
        } catch (...) {
            entries_.pop_back();
            throw;
        }
        return true;
    }

    std::size_t reap() {
        std::vector<Entry> finished;
        {
            std::lock_guard<std::mutex> lock(mu_);
            finished.reserve(entries_.size());
            for (std::size_t i = 0; i < entries_.size();) {
                if (!entries_[i].done->load(std::memory_order_acquire)) {
                    ++i;
                    continue;
                }
                finished.push_back(std::move(entries_[i]));
                if (i + 1 != entries_.size()) entries_[i] = std::move(entries_.back());
                entries_.pop_back();
            }
        }
        const auto count = finished.size();
        join_entries(finished);
        return count;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_.size();
    }

    void join_all() {
        std::vector<Entry> current;
        {
            std::lock_guard<std::mutex> lock(mu_);
            current.swap(entries_);
        }
        join_entries(current);
    }

    // Terminal operation. Close admission before joining, including work that
    // races shutdown; no accepted worker can escape the collection.
    void shutdown() {
        std::vector<Entry> current;
        {
            std::lock_guard<std::mutex> lock(mu_);
            closed_ = true;
            current.swap(entries_);
        }
        join_entries(current);
    }

private:
    struct Entry {
        // Shared by the collection and worker; never references the owner.
        std::shared_ptr<std::atomic<bool>> done;
        JoiningThread thread;
    };

    static void join_entries(std::vector<Entry>& entries) {
        for (auto& entry : entries) entry.thread.request_stop();
        for (auto& entry : entries) entry.thread.join();
    }

    mutable std::mutex mu_;
    bool closed_ = false;
    std::vector<Entry> entries_;
};

}  // namespace acecode
