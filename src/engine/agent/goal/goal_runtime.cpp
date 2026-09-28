#include "goal_runtime.hpp"

#include "agent/agent_callbacks.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/worker/agent_task_queue.hpp"
#include "permissions/permissions.hpp"
#include "session/event_dispatcher.hpp"
#include "session/session_manager.hpp"
#include "session/session_rewind.hpp"
#include "session/system_notice.hpp"
#include "utils/abort_signal.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <utility>

namespace acecode::agent {

using detail::format_goal_status_chip;

void GoalRuntime::begin_turn() {
    pending_budget_.store(false);
    pending_objective_.store(false);
}

void GoalRuntime::restore(SessionManager* session) {
    {
        std::lock_guard<std::mutex> lock(cursor_mu_);
        thread_id_.clear();
        goal_id_.clear();
        checkpoint_ = {};
    }
    if (!session) return;

    const std::string sid = session->current_session_id();
    ThreadGoalStore* store = session->existing_goal_store();
    if (!store || sid.empty()) return;

    std::string error;
    auto goal = store->get_thread_goal(sid, &error);
    if (!error.empty()) {
        LOG_WARN("[goal] failed to restore runtime state: " + error);
        return;
    }
    if (!goal.has_value() || goal->status != ThreadGoalStatus::Active) return;
    std::lock_guard<std::mutex> lock(cursor_mu_);
    thread_id_ = sid;
    goal_id_ = goal->goal_id;
    checkpoint_ = std::chrono::steady_clock::now();
}

void GoalRuntime::publish(SessionManager* session) {
    if (!session) {
        if (callbacks_.on_goal_status) callbacks_.on_goal_status(std::string{});
        return;
    }

    const std::string sid = session->current_session_id();
    if (sid.empty()) {
        if (callbacks_.on_goal_status) callbacks_.on_goal_status(std::string{});
        return;
    }

    ThreadGoalStore* store = session->existing_goal_store();
    if (!store) {
        emit_cleared(sid);
        return;
    }

    std::string error;
    auto goal = store->get_thread_goal(sid, &error);
    if (!error.empty()) {
        LOG_WARN("[goal] failed to publish current goal state: " + error);
        if (callbacks_.on_goal_status) callbacks_.on_goal_status(std::string{});
        return;
    }
    if (goal.has_value()) {
        emit_updated(*goal);
    } else {
        emit_cleared(sid);
    }
}

void GoalRuntime::emit_updated(const ThreadGoal& goal) {
    events_.emit(SessionEventKind::GoalUpdated,
        nlohmann::json{{"session_id", goal.thread_id}, {"goal", thread_goal_to_json(goal)}});
    if (callbacks_.on_goal_status) {
        callbacks_.on_goal_status(format_goal_status_chip(goal));
    }
    std::lock_guard<std::mutex> lock(cursor_mu_);
    if (goal.status == ThreadGoalStatus::Active) {
        thread_id_ = goal.thread_id;
        goal_id_ = goal.goal_id;
        checkpoint_ = std::chrono::steady_clock::now();
    } else if (goal.goal_id == goal_id_) {
        thread_id_.clear();
        goal_id_.clear();
        checkpoint_ = {};
    }
}

void GoalRuntime::emit_cleared(const std::string& session_id) {
    events_.emit(SessionEventKind::GoalCleared,
        nlohmann::json{{"session_id", session_id}});
    if (callbacks_.on_goal_status) callbacks_.on_goal_status(std::string{});
    std::lock_guard<std::mutex> lock(cursor_mu_);
    if (session_id == thread_id_) {
        thread_id_.clear();
        goal_id_.clear();
        checkpoint_ = {};
    }
}

void GoalRuntime::emit_todo_updated(SessionManager* session, const nlohmann::json& payload) {
    nlohmann::json event_payload = payload.is_object()
        ? payload
        : nlohmann::json::object();
    if (!event_payload.contains("session_id") && session) {
        const std::string sid = session->current_session_id();
        if (!sid.empty()) event_payload["session_id"] = sid;
    }
    events_.emit(SessionEventKind::TodoUpdated, event_payload);
    if (callbacks_.on_todo_updated) {
        callbacks_.on_todo_updated(event_payload);
    }
}

void GoalRuntime::account_usage(SessionManager* session, std::int64_t token_delta, bool allow_complete) {
    if (!session) return;
    const std::string sid = session->current_session_id();
    ThreadGoalStore* store = session->existing_goal_store();
    if (!store || sid.empty()) return;

    bool restore_needed;
    {
        std::lock_guard<std::mutex> lock(cursor_mu_);
        restore_needed = thread_id_ != sid || goal_id_.empty();
    }
    if (restore_needed) restore(session);

    std::string accounted_goal_id;
    std::int64_t elapsed_seconds = 0;
    {
        std::lock_guard<std::mutex> lock(cursor_mu_);
        if (thread_id_ != sid || goal_id_.empty()) return;
        accounted_goal_id = goal_id_;
        const auto now = std::chrono::steady_clock::now();
        if (checkpoint_.time_since_epoch().count() != 0) {
            elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - checkpoint_).count();
        }
        checkpoint_ = now;
    }

    std::string error;
    auto result = store->account_thread_goal_usage(
        sid,
        accounted_goal_id,
        std::max<std::int64_t>(0, token_delta),
        elapsed_seconds,
        allow_complete,
        &error);
    if (!error.empty()) {
        LOG_WARN("[goal] accounting failed: " + error);
        return;
    }
    if (!result.goal.has_value()) return;

    if (result.became_budget_limited) {
        emit_updated(*result.goal);
        bool publish_notice = false;
        {
            std::lock_guard<std::mutex> lock(cursor_mu_);
            if (budget_notice_id_ != result.goal->goal_id) {
                budget_notice_id_ = result.goal->goal_id;
                publish_notice = true;
            }
        }
        if (publish_notice) {
            transcript_.dispatch_message("system", "[Goal] Token budget reached; automatic continuation stopped.", false,
                make_system_notice_metadata("goal_budget_reached", {{"goal", thread_goal_to_json(*result.goal)}}), nlohmann::json::array());
            // 让运行中的回合在下一次模型请求前收到 wrap-up 提示(对齐 Codex
            // budget_limit steering):总结进展、指出剩余工作,不再开新活。
            pending_budget_.store(true);
        }
        return;
    }

    if (result.updated) {
        emit_updated(*result.goal);
    }
}

void GoalRuntime::maybe_continue(SessionManager* session, detail::GoalPromptTools tools) {
    if (!session || abort_.raw().load() || busy_.load()) return;
    if (!tools.update_goal) return;
    // Plan mode 下不自动开新回合(对齐 Codex try_start_turn_if_idle 的
    // PlanMode 拒绝):plan 模式的只读约束不该被 goal continuation 绕过。
    // 退出 plan mode 后的下一次回合结束会重新触发 continuation。
    if (permissions_.mode() == PermissionMode::Plan) {
        return;
    }
    const std::string sid = session->current_session_id();
    ThreadGoalStore* store = session->existing_goal_store();
    if (!store || sid.empty()) return;

    std::string error;
    auto goal = store->get_thread_goal(sid, &error);
    if (!error.empty()) {
        LOG_WARN("[goal] failed to load goal for continuation: " + error);
        return;
    }
    if (!goal.has_value() || goal->status != ThreadGoalStatus::Active) return;

    const bool queued = queue_.with_locked([&](agent::AgentTaskQueue::Locked& queue) {
        if (queue.stopped() || !queue.empty()) return false;
        WorkerTask task;
        task.kind = WorkerTask::Kind::Chat;
        task.input.text = detail::build_goal_context_prompt(*goal, tools);
        task.hidden_goal_context = true;
        queue.push(std::move(task));
        return true;
    });
    if (queued) queue_.notify();
}

bool GoalRuntime::unattended_active(SessionManager* session) {
    if (!session) return false;
    // Plan mode 的只读约束优先于 goal 自动放行,否则 plan 模式形同虚设。
    if (permissions_.mode() == PermissionMode::Plan) {
        return false;
    }
    ThreadGoalStore* store = session->existing_goal_store();
    if (!store) return false;
    auto is_active = [store](const std::string& sid) {
        if (sid.empty()) return false;
        auto goal = store->get_thread_goal(sid);
        return goal.has_value() && goal->status == ThreadGoalStatus::Active;
    };
    if (is_active(session->current_session_id())) return true;
    // 子代理会话与父会话共享同一个项目级 goal store:父会话的 active goal
    // 意味着整条链路无人值守 —— 子代理的权限确认会冒泡到父 UI,同样必须
    // 自动放行,否则 goal 回合里 spawn 的子代理照样弹窗。
    return is_active(session->current_parent_session_id());
}

void GoalRuntime::notify_objective_updated() {
    if (!busy_.load()) return;
    pending_objective_.store(true);
}

void GoalRuntime::stop_after_error(SessionManager* session, const ProviderErrorInfo& info) {
    if (!session) return;
    const std::string sid = session->current_session_id();
    ThreadGoalStore* store = session->existing_goal_store();
    if (!store || sid.empty()) return;

    std::string error;
    auto goal = store->get_thread_goal(sid, &error);
    if (!error.empty()) {
        LOG_WARN("[goal] failed to load goal after turn error: " + error);
        return;
    }
    if (!goal.has_value() || goal->status != ThreadGoalStatus::Active) return;

    // 先入账已消耗的 usage,再停 goal;入账可能把 goal 翻成 budget_limited,
    // 那种情况下预算逻辑已经接管,不再叠加错误状态。
    account_usage(session, 0, false);
    goal = store->get_thread_goal(sid, &error);
    if (!goal.has_value() || goal->status != ThreadGoalStatus::Active) return;

    const bool usage_limited = info.status_code == 429;
    const ThreadGoalStatus next = usage_limited
        ? ThreadGoalStatus::UsageLimited
        : ThreadGoalStatus::Blocked;
    if (!store->update_thread_goal_status(sid, goal->goal_id, next, &error)) {
        LOG_WARN("[goal] failed to stop goal after turn error: " + error);
        return;
    }
    auto updated = store->get_thread_goal(sid);
    if (updated.has_value()) emit_updated(*updated);
    transcript_.dispatch_message("system",
        usage_limited
            ? "[Goal] Provider usage limit hit; goal marked usage_limited and automatic continuation stopped. Use /goal resume to continue later."
            : "[Goal] Turn ended with an error; goal marked blocked and automatic continuation stopped. Use /goal resume to retry.",
        false, make_system_notice_metadata(usage_limited ? "goal_usage_limited" : "goal_blocked"), nlohmann::json::array());
    LOG_WARN("[goal] stopped active goal after turn error: status=" +
             to_string(next) + " provider_status_code=" +
             std::to_string(info.status_code));
}

void GoalRuntime::inject_steering(SessionManager* session, detail::GoalPromptTools tools) {
    const bool budget = pending_budget_.exchange(false);
    const bool objective = pending_objective_.exchange(false);
    if (!budget && !objective) return;
    if (!session) return;
    const std::string sid = session->current_session_id();
    ThreadGoalStore* store = session->existing_goal_store();
    if (!store || sid.empty()) return;
    auto goal = store->get_thread_goal(sid);
    if (!goal.has_value()) return;

    auto append_hidden = [this, session](const std::string& text) {
        ChatMessage msg;
        msg.role = "user";
        msg.content = text;
        msg.metadata = nlohmann::json{{"hidden_goal_context", true}};
        ensure_user_message_identity(msg);
        history_.append(msg);
        if (session) session->on_message(msg);
    };

    if (budget && goal->status == ThreadGoalStatus::BudgetLimited) {
        append_hidden(detail::build_goal_budget_limit_prompt(*goal, tools));
        LOG_INFO("[goal] injected budget_limit steering into active turn");
    }
    if (objective && goal->status == ThreadGoalStatus::Active) {
        append_hidden(detail::build_goal_objective_updated_prompt(*goal, tools));
        LOG_INFO("[goal] injected objective_updated steering into active turn");
    }
}

} // namespace acecode::agent
