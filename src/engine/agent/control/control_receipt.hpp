#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace acecode {

// Authoritative receipt for a control task inserted into the AgentLoop worker
// queue. queued_behind_turn is computed while holding the same mutex that
// orders chat/control tasks, so a chat that has been submitted but has not yet
// flipped busy=true is still observed. Completion and success are separate: a
// callback that ran but could not commit its state must not be reported as
// applied.
struct ControlExecutionState {
    mutable std::mutex mu;
    std::condition_variable cv;
    bool completed = false;
    bool cancelled = false;
    bool succeeded = false;
};

struct ControlEnqueueReceipt {
    std::uint64_t sequence = 0;
    bool accepted = false;
    bool queued_behind_turn = false;
    std::shared_ptr<ControlExecutionState> execution;

    bool completed() const {
        if (!execution) return false;
        std::lock_guard<std::mutex> lock(execution->mu);
        return execution->completed;
    }

    bool cancelled() const {
        if (!execution) return false;
        std::lock_guard<std::mutex> lock(execution->mu);
        return execution->cancelled;
    }

    bool succeeded() const {
        if (!execution) return false;
        std::lock_guard<std::mutex> lock(execution->mu);
        return execution->completed && execution->succeeded;
    }

    bool applied() const {
        return succeeded();
    }

    bool wait_for_completion(std::chrono::milliseconds timeout) const {
        if (!execution) return false;
        std::unique_lock<std::mutex> lock(execution->mu);
        execution->cv.wait_for(lock, timeout, [&] {
            return execution->completed || execution->cancelled;
        });
        return execution->completed;
    }
};

} // namespace acecode
