#pragma once

#include "agent/goal/goal_prompts.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace acecode {
class SessionManager;
class PermissionManager;
class EventDispatcher;
class AbortSignal;
struct AgentCallbacks;
struct ProviderErrorInfo;
}

namespace acecode::agent {
class AgentTaskQueue;
class TranscriptWriter;
class ConversationHistory;

// account_usage / emit_updated / emit_cleared can enter from parallel read
// tools; objective notifications also enter from UI/API control threads.
// cursor_mu_ is a leaf: copy/reserve cursor values, then release it before any
// store, event, callback or queue operation. Steering is consumed by the worker.
class GoalRuntime {
public:
    GoalRuntime(AgentTaskQueue& queue, ConversationHistory& history, TranscriptWriter& transcript,
                EventDispatcher& events, AgentCallbacks& callbacks, PermissionManager& permissions,
                const std::atomic<bool>& busy, AbortSignal& abort)
        : queue_(queue), history_(history), transcript_(transcript), events_(events),
          callbacks_(callbacks), permissions_(permissions), busy_(busy), abort_(abort) {}
    void restore(SessionManager* session);
    void publish(SessionManager* session);
    void emit_updated(const ThreadGoal& goal);
    void emit_cleared(const std::string& session_id);
    void emit_todo_updated(SessionManager* session, const nlohmann::json& payload);
    void account_usage(SessionManager* session, std::int64_t token_delta, bool allow_complete);
    void maybe_continue(SessionManager* session, detail::GoalPromptTools tools);
    bool unattended_active(SessionManager* session);
    void notify_objective_updated();
    void stop_after_error(SessionManager* session, const ProviderErrorInfo& info);
    void inject_steering(SessionManager* session, detail::GoalPromptTools tools);
    void begin_turn();

private:
    AgentTaskQueue& queue_;
    ConversationHistory& history_;
    TranscriptWriter& transcript_;
    EventDispatcher& events_;
    AgentCallbacks& callbacks_;
    PermissionManager& permissions_;
    const std::atomic<bool>& busy_;
    AbortSignal& abort_;
    mutable std::mutex cursor_mu_;
    std::string thread_id_;
    std::string goal_id_;
    std::string budget_notice_id_;
    std::chrono::steady_clock::time_point checkpoint_{};
    std::atomic<bool> pending_budget_{false};
    std::atomic<bool> pending_objective_{false};
};

} // namespace acecode::agent
