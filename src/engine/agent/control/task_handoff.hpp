#pragma once

#include "session/thread_goal_store.hpp"

#include <functional>
#include <optional>
#include <string>

namespace acecode { class SessionManager; }

namespace acecode::agent {
class AgentTaskQueue;

struct TaskHandoffResult {
    bool accepted = false;
    std::optional<ThreadGoal> paused_goal;
};

// Synchronous transaction, source.queue -> target.queue. Target acceptance must
// not call back into the source queue/gate; no queue lock spans emitted events.
class TaskHandoff {
public:
    explicit TaskHandoff(AgentTaskQueue& source) : source_(source) {}
    bool try_start_side_task(const std::function<bool()>& accept_target, std::string* error);
    TaskHandoffResult complete(SessionManager* session, const std::string& target_session_id,
                               const std::function<bool()>& accept_target, std::string* error);
private:
    AgentTaskQueue& source_;
};

} // namespace acecode::agent
