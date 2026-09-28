#pragma once

#include "agent/control/control_receipt.hpp"
#include "agent/turn/turn_types.hpp"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace acecode { class AbortSignal; }

namespace acecode::agent {

// Lock order: ActiveTurnGate -> queue; handoff source.queue -> target.queue.
// A queue-locked callback must never enter ActiveTurnGate or take a model lock.
// Ordinary and priority work each retain FIFO order. Shutdown only closes the
// consumer at this migration step; release of queued captures belongs to O-11.
class AgentTaskQueue {
public:
    explicit AgentTaskQueue(const std::atomic<bool>& busy) : busy_(busy) {}

    class Locked {
    public:
        Locked(const Locked&) = delete;
        Locked& operator=(const Locked&) = delete;
        bool stopped() const { return queue_.shutdown_requested_; }
        bool empty() const;
        bool worker_active() const { return queue_.worker_task_active_; }
        bool active_operation() const;
        bool idle() const;
        bool has_user_work() const;
        bool has_turn_before_control() const;
        bool has_suggestion(const std::string& id) const;
        void remember_suggestion(const std::string& id);
        std::size_t priority_size() const { return queue_.priority_.size(); }
        void push(WorkerTask task, bool priority = false);
        void remove_goal_continuations();
        std::uint64_t next_control_sequence() { return ++queue_.next_control_sequence_; }

    private:
        friend class AgentTaskQueue;
        explicit Locked(AgentTaskQueue& queue) : queue_(queue) {}
        AgentTaskQueue& queue_;
    };

    template <typename Fn>
    decltype(auto) with_locked(Fn&& fn) {
        Lock lock(*this);
        Locked state(*this);
        // The callback and Locked view are borrowed for this call only.
        return std::forward<Fn>(fn)(state);
    }

    void enqueue(WorkerTask task);
    ControlEnqueueReceipt enqueue_control(std::function<bool()> control);
    bool enqueue_suggestion(const UserInput& input, const std::string& id, AbortSignal& abort);
    bool has_pending_work();
    bool has_user_work();
    bool has_suggestion(const std::string& id);
    bool try_run_idle(const std::function<void()>& control);
    bool wait_pop(WorkerTask& task);
    void finish_task();
    void request_shutdown();
    void notify() { cv_.notify_one(); }
    bool held_by_current_thread() const;
    bool on_worker_thread() const { return worker_queue_ == this; }

private:
    class Lock {
    public:
        explicit Lock(AgentTaskQueue& queue);
        ~Lock();
        std::unique_lock<std::mutex>& native() { return lock_; }
    private:
        AgentTaskQueue& queue_;
        std::unique_lock<std::mutex> lock_;
    };
    static bool is_turn(WorkerTask::Kind kind);
    static bool is_user_work(const WorkerTask& task);
    static thread_local std::vector<const AgentTaskQueue*> held_;
    static thread_local const AgentTaskQueue* worker_queue_;
    const std::atomic<bool>& busy_; // Constructor-injected, outlives the queue.
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<WorkerTask> priority_;
    std::deque<WorkerTask> ordinary_;
    std::set<std::string> suggestion_ids_;
    bool shutdown_requested_ = false;
    bool worker_task_active_ = false;
    WorkerTask::Kind worker_task_kind_ = WorkerTask::Kind::Control;
    std::uint64_t next_control_sequence_ = 0;
};

} // namespace acecode::agent
