#include "agent/agent_loop.hpp"
#include "session/session_storage.hpp"
#include "utils/logger.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode {

bool AgentLoop::has_pending_work() {
    std::lock_guard<std::mutex> lock(queue_mu_);
    return busy_.load() || worker_task_active_ || !task_queue_.empty() || !priority_task_queue_.empty();
}

bool AgentLoop::has_queued_user_work() {
    std::lock_guard<std::mutex> lock(queue_mu_);
    return has_queued_user_work_locked();
}

bool AgentLoop::has_queued_user_work_locked() const {
    auto has_user_work = [](std::queue<WorkerTask> queue) {
        while (!queue.empty()) {
            const auto& task = queue.front();
            if ((task.kind == WorkerTask::Kind::Chat && !task.hidden_goal_context) ||
                task.kind == WorkerTask::Kind::Shell || task.kind == WorkerTask::Kind::Compact) return true;
            queue.pop();
        }
        return false;
    };
    return has_user_work(task_queue_) || has_user_work(priority_task_queue_);
}

bool AgentLoop::has_task_suggestion_input(const std::string& suggestion_id) {
    std::lock_guard<std::mutex> lock(queue_mu_);
    return task_suggestion_input_ids_.count(suggestion_id) != 0;
}

} // namespace acecode
