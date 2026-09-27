#pragma once

#include "utils/abort_signal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace acecode {

namespace abandonable_detail {

void start_owned_work(std::string name, std::function<void()> fn);

template <typename R>
using StoredResult = std::conditional_t<std::is_void_v<R>, bool, R>;

template <typename R>
struct ResultState {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    bool abandoned = false;
    std::unique_ptr<StoredResult<R>> result;
    std::exception_ptr error;
};

}  // namespace abandonable_detail

// The function must own all its inputs, including everything reachable from
// them. It must not borrow a host, service, logger, or stack frame that can end
// before it returns. Exceptions are contained and logged by the worker.
template <typename Fn>
void spawn_owned_detached(std::string name, Fn&& fn) {
    // Shared by the type-erased launcher and worker; allows move-only closures.
    auto task = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
    abandonable_detail::start_owned_work(std::move(name), [task = std::move(task)]() mutable {
        std::invoke(*task);
    });
}

// True means every registered function and its captures have been destroyed.
// False means the deadline expired; workers retain their own registry lease.
// Stop work producers before waiting; this function does not close admission.
bool wait_for_abandoned_work(std::chrono::steady_clock::time_point deadline);

template <typename Rep, typename Period>
bool wait_for_abandoned_work(std::chrono::duration<Rep, Period> timeout) {
    return wait_for_abandoned_work(std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout));
}

template <typename R>
using AbandonableResult = std::conditional_t<std::is_void_v<R>, bool, std::optional<R>>;

// nullopt (or false for void) means abandoned. Otherwise the value or exception
// is delivered to this caller only. With no abort flag, execution stays inline.
// poll is an upper bound; cap legacy atomic polling to 25 ms to keep cancellation
// below the 100 ms contract even when the caller retains the 100 ms default.
template <typename R, typename Fn>
AbandonableResult<R> run_abandonable(
    Fn&& fn, const std::atomic<bool>* abort,
    std::chrono::milliseconds poll = std::chrono::milliseconds(100)) {
    static_assert(!std::is_reference_v<R>, "Abandonable results must own their value");
    if (poll <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("run_abandonable requires a positive poll interval");
    }
    if (!abort) {
        if constexpr (std::is_void_v<R>) {
            std::invoke(std::forward<Fn>(fn));
            return true;
        } else {
            return std::invoke(std::forward<Fn>(fn));
        }
    }
    if (abort->load(std::memory_order_acquire)) return {};

    // Shared by the caller and the detached worker; neither captures the abort
    // flag. The flag is borrowed only until this synchronous function returns.
    auto state = std::make_shared<abandonable_detail::ResultState<R>>();
    spawn_owned_detached("run_abandonable",
        [state, task = std::decay_t<Fn>(std::forward<Fn>(fn))]() mutable {
            std::unique_ptr<abandonable_detail::StoredResult<R>> result;
            std::exception_ptr error;
            try {
                if constexpr (std::is_void_v<R>) {
                    std::invoke(task);
                    result = std::make_unique<bool>(true);
                } else {
                    result = std::make_unique<R>(std::invoke(task));
                }
            } catch (...) {
                error = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lock(state->mu);
                if (!state->abandoned) {
                    state->result = std::move(result);
                    state->error = std::move(error);
                    state->done = true;
                }
            }
            state->cv.notify_all();
            // A late result (and its exception, if any) is destroyed outside
            // the mutex and never published back into a destroyed caller.
        });

    std::unique_lock<std::mutex> lock(state->mu);
    while (!state->done) {
        if (abort->load(std::memory_order_acquire)) {
            state->abandoned = true;
            return {};
        }
        state->cv.wait_for(lock, (std::min)(poll, std::chrono::milliseconds(25)));
    }
    auto result = std::move(state->result);
    auto error = std::move(state->error);
    lock.unlock();
    if (error) std::rethrow_exception(error);
    if constexpr (std::is_void_v<R>) {
        return true;
    } else {
        return std::move(*result);
    }
}

template <typename R, typename Fn>
AbandonableResult<R> run_abandonable(
    Fn&& fn, const std::atomic<bool>& abort,
    std::chrono::milliseconds poll = std::chrono::milliseconds(100)) {
    return run_abandonable<R>(std::forward<Fn>(fn), &abort, poll);
}

template <typename R, typename Fn>
AbandonableResult<R> run_abandonable(
    Fn&& fn, const AbortSignal& abort,
    std::chrono::milliseconds poll = std::chrono::milliseconds(100)) {
    return run_abandonable<R>(std::forward<Fn>(fn), &abort.raw(), poll);
}

template <typename Fn, typename Abort>
auto run_abandonable(Fn&& fn, const Abort& abort,
                     std::chrono::milliseconds poll = std::chrono::milliseconds(100))
    -> AbandonableResult<std::invoke_result_t<std::decay_t<Fn>&>> {
    return run_abandonable<std::invoke_result_t<std::decay_t<Fn>&>>(
        std::forward<Fn>(fn), abort, poll);
}

}  // namespace acecode
