#include "agent_task_queue.hpp"

#include "utils/abort_signal.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <cassert>
#include <exception>

namespace acecode::agent {

thread_local std::vector<const AgentTaskQueue*> AgentTaskQueue::held_;
thread_local const AgentTaskQueue* AgentTaskQueue::worker_queue_ = nullptr;

AgentTaskQueue::Lock::Lock(AgentTaskQueue& queue) : queue_(queue), lock_(queue.mu_) {
    held_.push_back(&queue_);
}

AgentTaskQueue::Lock::~Lock() {
    assert(!held_.empty() && held_.back() == &queue_);
    held_.pop_back();
}

bool AgentTaskQueue::held_by_current_thread() const {
    return std::find(held_.begin(), held_.end(), this) != held_.end();
}

bool AgentTaskQueue::is_turn(WorkerTask::Kind kind) {
    return kind == WorkerTask::Kind::Chat || kind == WorkerTask::Kind::Shell ||
           kind == WorkerTask::Kind::Compact;
}

bool AgentTaskQueue::is_user_work(const WorkerTask& task) {
    return (task.kind == WorkerTask::Kind::Chat && !task.hidden_goal_context) ||
           task.kind == WorkerTask::Kind::Shell || task.kind == WorkerTask::Kind::Compact;
}

bool AgentTaskQueue::Locked::empty() const {
    return queue_.priority_.empty() && queue_.ordinary_.empty();
}

bool AgentTaskQueue::Locked::active_operation() const {
    return queue_.busy_.load() ||
           (queue_.worker_task_active_ && queue_.worker_task_kind_ != WorkerTask::Kind::Control);
}

bool AgentTaskQueue::Locked::idle() const {
    return !stopped() && !queue_.busy_.load() && !worker_active() && empty();
}

bool AgentTaskQueue::Locked::has_user_work() const {
    return std::any_of(queue_.ordinary_.begin(), queue_.ordinary_.end(), is_user_work) ||
           std::any_of(queue_.priority_.begin(), queue_.priority_.end(), is_user_work);
}

bool AgentTaskQueue::Locked::has_turn_before_control() const {
    const auto turn = [](const WorkerTask& task) { return is_turn(task.kind); };
    return (queue_.worker_task_active_ && is_turn(queue_.worker_task_kind_)) ||
           std::any_of(queue_.priority_.begin(), queue_.priority_.end(), turn) ||
           std::any_of(queue_.ordinary_.begin(), queue_.ordinary_.end(), turn);
}

bool AgentTaskQueue::Locked::has_suggestion(const std::string& id) const {
    return queue_.suggestion_ids_.count(id) != 0;
}

void AgentTaskQueue::Locked::remember_suggestion(const std::string& id) {
    queue_.suggestion_ids_.insert(id);
}

void AgentTaskQueue::Locked::push(WorkerTask task, bool priority) {
    (priority ? queue_.priority_ : queue_.ordinary_).push_back(std::move(task));
}

void AgentTaskQueue::Locked::remove_goal_continuations() {
    const auto hidden_goal = [](const WorkerTask& task) {
        return task.kind == WorkerTask::Kind::Chat && task.hidden_goal_context;
    };
    for (auto* queue : {&queue_.ordinary_, &queue_.priority_}) {
        queue->erase(std::remove_if(queue->begin(), queue->end(), hidden_goal), queue->end());
    }
}

void AgentTaskQueue::enqueue(WorkerTask task) {
    with_locked([&](Locked& state) { state.push(std::move(task)); });
    notify();
}

bool AgentTaskQueue::has_pending_work() {
    return with_locked([&](Locked& state) {
        return busy_.load() || state.worker_active() || !state.empty();
    });
}

bool AgentTaskQueue::has_user_work() {
    return with_locked([](Locked& state) { return state.has_user_work(); });
}

bool AgentTaskQueue::has_suggestion(const std::string& id) {
    return with_locked([&](Locked& state) { return state.has_suggestion(id); });
}

bool AgentTaskQueue::try_run_idle(const std::function<void()>& control) {
    if (!control) return false;
    return with_locked([&](Locked& state) {
        if (!state.idle()) return false;
        control();
        return true;
    });
}

bool AgentTaskQueue::wait_pop(WorkerTask& task) {
    worker_queue_ = this;
    Lock lock(*this);
    cv_.wait(lock.native(), [this] {
        return !priority_.empty() || !ordinary_.empty() || shutdown_requested_;
    });
    if (shutdown_requested_) {
        worker_queue_ = nullptr;
        return false;
    }
    auto& queue = priority_.empty() ? ordinary_ : priority_;
    task = std::move(queue.front());
    queue.pop_front();
    worker_task_active_ = true;
    worker_task_kind_ = task.kind;
    return true;
}

void AgentTaskQueue::finish_task() {
    Lock lock(*this);
    worker_task_active_ = false;
    worker_task_kind_ = WorkerTask::Kind::Control;
}

void AgentTaskQueue::request_shutdown() {
    Lock lock(*this);
    shutdown_requested_ = true;
}

bool AgentTaskQueue::enqueue_suggestion(
    const UserInput& input, const std::string& id, AbortSignal& abort) {
    if (id.empty() || input.empty()) return false;
    const bool accepted = with_locked([&](Locked& state) {
        if (state.stopped()) return false;
        if (state.has_suggestion(id)) return true;
        if (state.active_operation() || state.has_user_work()) return false;
        WorkerTask task;
        task.kind = WorkerTask::Kind::Chat;
        task.input = input;
        if (!task.input.metadata.is_object()) task.input.metadata = nlohmann::json::object();
        task.input.metadata["task_suggestion_id"] = id;
        task.hidden_goal_context = false;
        state.push(std::move(task));
        state.remember_suggestion(id);
        abort.clear();
        return true;
    });
    if (accepted) notify();
    return accepted;
}

ControlEnqueueReceipt AgentTaskQueue::enqueue_control(
    std::function<bool()> control) {
    ControlEnqueueReceipt receipt;
    if (!control) return receipt;
    auto execution = std::make_shared<ControlExecutionState>();
    const bool accepted = with_locked([&](Locked& state) {
        if (state.stopped()) return false;
        const bool queued_behind_turn = state.has_turn_before_control();

        WorkerTask task;
        task.kind = WorkerTask::Kind::Control;
        task.control = [control = std::move(control), execution]() mutable {
            bool succeeded = false;
            try {
                succeeded = control();
            } catch (const std::exception& e) {
                LOG_ERROR(std::string("Control task failed: ") + e.what());
            } catch (...) {
                LOG_ERROR("Control task failed with unknown exception");
            }
            {
                std::lock_guard<std::mutex> lock(execution->mu);
                execution->succeeded = succeeded;
                execution->completed = true;
            }
            execution->cv.notify_all();
        };
        state.push(std::move(task));
        receipt.sequence = state.next_control_sequence();
        receipt.accepted = true;
        receipt.queued_behind_turn = queued_behind_turn;
        receipt.execution = std::move(execution);
        return true;
    });
    if (accepted) notify();
    return receipt;
}

} // namespace acecode::agent
