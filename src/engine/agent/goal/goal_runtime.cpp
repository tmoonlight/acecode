#include "agent/agent_loop.hpp"
#include "agent/worker/agent_task_queue.hpp"
#include "agent/goal/goal_prompts.hpp"
#include "hooks/hook_runtime.hpp"
#include "llm/tool_protocol_names.hpp"
#include "pa/pa_overflow_rescue.hpp"
#include "permissions/interaction_mode.hpp"
#include "permissions/shell_write_guard.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_rewind.hpp"
#include "session/session_storage.hpp"
#include "session/system_notice.hpp"
#include "session/thread_goal_store.hpp"
#include "session/thread_repair.hpp"
#include "session/token_tracker.hpp"
#include "session/turn_timing.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"
#include "utils/time.hpp"
#include "utils/uuid.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode {

using agent::detail::format_goal_status_chip;

void AgentLoop::restore_goal_runtime() {
    goal_accounting_thread_id_.clear();
    goal_accounting_goal_id_.clear();
    goal_time_checkpoint_ = {};
    if (!session_manager_) return;

    const std::string sid = session_manager_->current_session_id();
    ThreadGoalStore* store = session_manager_->existing_goal_store();
    if (!store || sid.empty()) return;

    std::string error;
    auto goal = store->get_thread_goal(sid, &error);
    if (!error.empty()) {
        LOG_WARN("[goal] failed to restore runtime state: " + error);
        return;
    }
    if (!goal.has_value() || goal->status != ThreadGoalStatus::Active) return;
    goal_accounting_thread_id_ = sid;
    goal_accounting_goal_id_ = goal->goal_id;
    goal_time_checkpoint_ = std::chrono::steady_clock::now();
}

void AgentLoop::publish_current_goal_state() {
    if (!session_manager_) {
        if (callbacks_.on_goal_status) callbacks_.on_goal_status(std::string{});
        return;
    }

    const std::string sid = session_manager_->current_session_id();
    if (sid.empty()) {
        if (callbacks_.on_goal_status) callbacks_.on_goal_status(std::string{});
        return;
    }

    ThreadGoalStore* store = session_manager_->existing_goal_store();
    if (!store) {
        emit_goal_cleared(sid);
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
        emit_goal_updated(*goal);
    } else {
        emit_goal_cleared(sid);
    }
}

void AgentLoop::emit_goal_updated(const ThreadGoal& goal) {
    events_.emit(SessionEventKind::GoalUpdated,
        nlohmann::json{{"session_id", goal.thread_id}, {"goal", thread_goal_to_json(goal)}});
    if (callbacks_.on_goal_status) {
        callbacks_.on_goal_status(format_goal_status_chip(goal));
    }
    if (goal.status == ThreadGoalStatus::Active) {
        goal_accounting_thread_id_ = goal.thread_id;
        goal_accounting_goal_id_ = goal.goal_id;
        goal_time_checkpoint_ = std::chrono::steady_clock::now();
    } else if (goal.goal_id == goal_accounting_goal_id_) {
        goal_accounting_thread_id_.clear();
        goal_accounting_goal_id_.clear();
        goal_time_checkpoint_ = {};
    }
}

void AgentLoop::emit_goal_cleared(const std::string& session_id) {
    events_.emit(SessionEventKind::GoalCleared,
        nlohmann::json{{"session_id", session_id}});
    if (callbacks_.on_goal_status) callbacks_.on_goal_status(std::string{});
    if (session_id == goal_accounting_thread_id_) {
        goal_accounting_thread_id_.clear();
        goal_accounting_goal_id_.clear();
        goal_time_checkpoint_ = {};
    }
}

void AgentLoop::emit_todo_updated(const nlohmann::json& payload) {
    nlohmann::json event_payload = payload.is_object()
        ? payload
        : nlohmann::json::object();
    if (!event_payload.contains("session_id") && session_manager_) {
        const std::string sid = session_manager_->current_session_id();
        if (!sid.empty()) event_payload["session_id"] = sid;
    }
    events_.emit(SessionEventKind::TodoUpdated, event_payload);
    if (callbacks_.on_todo_updated) {
        callbacks_.on_todo_updated(event_payload);
    }
}

void AgentLoop::account_goal_usage(std::int64_t token_delta, bool allow_complete) {
    if (!session_manager_) return;
    const std::string sid = session_manager_->current_session_id();
    ThreadGoalStore* store = session_manager_->existing_goal_store();
    if (!store || sid.empty()) return;

    if (goal_accounting_thread_id_ != sid || goal_accounting_goal_id_.empty()) {
        restore_goal_runtime();
    }
    if (goal_accounting_thread_id_ != sid || goal_accounting_goal_id_.empty()) return;

    const auto now = std::chrono::steady_clock::now();
    std::int64_t elapsed_seconds = 0;
    if (goal_time_checkpoint_.time_since_epoch().count() != 0) {
        elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            now - goal_time_checkpoint_).count();
    }
    goal_time_checkpoint_ = now;

    std::string error;
    auto result = store->account_thread_goal_usage(
        sid,
        goal_accounting_goal_id_,
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
        emit_goal_updated(*result.goal);
        if (budget_notice_goal_id_ != result.goal->goal_id) {
            budget_notice_goal_id_ = result.goal->goal_id;
            dispatch_message("system", "[Goal] Token budget reached; automatic continuation stopped.", false,
                make_system_notice_metadata("goal_budget_reached", {{"goal", thread_goal_to_json(*result.goal)}}));
            // 让运行中的回合在下一次模型请求前收到 wrap-up 提示(对齐 Codex
            // budget_limit steering):总结进展、指出剩余工作,不再开新活。
            pending_goal_budget_limit_steering_.store(true);
        }
        return;
    }

    if (result.updated) {
        emit_goal_updated(*result.goal);
    }
}

std::string AgentLoop::build_goal_context_prompt(const ThreadGoal& goal) const {
    return agent::detail::build_goal_context_prompt(goal, {
        tools_.is_allowed("update_goal", &tool_capability_policy_),
        tools_.is_allowed("AskUserQuestion", &tool_capability_policy_)});
}

std::string AgentLoop::build_goal_budget_limit_prompt(const ThreadGoal& goal) const {
    return agent::detail::build_goal_budget_limit_prompt(goal, {
        tools_.is_allowed("update_goal", &tool_capability_policy_),
        tools_.is_allowed("AskUserQuestion", &tool_capability_policy_)});
}

std::string AgentLoop::build_goal_objective_updated_prompt(const ThreadGoal& goal) const {
    return agent::detail::build_goal_objective_updated_prompt(goal, {
        tools_.is_allowed("update_goal", &tool_capability_policy_),
        tools_.is_allowed("AskUserQuestion", &tool_capability_policy_)});
}

void AgentLoop::maybe_continue_goal() {
    if (!session_manager_ || abort_signal_.raw().load() || busy_.load()) return;
    if (!tools_.is_allowed("update_goal", &tool_capability_policy_)) return;
    // Plan mode 下不自动开新回合(对齐 Codex try_start_turn_if_idle 的
    // PlanMode 拒绝):plan 模式的只读约束不该被 goal continuation 绕过。
    // 退出 plan mode 后的下一次回合结束会重新触发 continuation。
    if (permissions_.mode() == PermissionMode::Plan) {
        return;
    }
    const std::string sid = session_manager_->current_session_id();
    ThreadGoalStore* store = session_manager_->existing_goal_store();
    if (!store || sid.empty()) return;

    std::string error;
    auto goal = store->get_thread_goal(sid, &error);
    if (!error.empty()) {
        LOG_WARN("[goal] failed to load goal for continuation: " + error);
        return;
    }
    if (!goal.has_value() || goal->status != ThreadGoalStatus::Active) return;

    const bool queued = task_queue_->with_locked([&](agent::AgentTaskQueue::Locked& queue) {
        if (queue.stopped() || !queue.empty()) return false;
        WorkerTask task;
        task.kind = WorkerTask::Kind::Chat;
        task.input.text = build_goal_context_prompt(*goal);
        task.hidden_goal_context = true;
        queue.push(std::move(task));
        return true;
    });
    if (queued) task_queue_->notify();
}

bool AgentLoop::goal_unattended_active() {
    if (!session_manager_) return false;
    // Plan mode 的只读约束优先于 goal 自动放行,否则 plan 模式形同虚设。
    if (permissions_.mode() == PermissionMode::Plan) {
        return false;
    }
    ThreadGoalStore* store = session_manager_->existing_goal_store();
    if (!store) return false;
    auto is_active = [store](const std::string& sid) {
        if (sid.empty()) return false;
        auto goal = store->get_thread_goal(sid);
        return goal.has_value() && goal->status == ThreadGoalStatus::Active;
    };
    if (is_active(session_manager_->current_session_id())) return true;
    // 子代理会话与父会话共享同一个项目级 goal store:父会话的 active goal
    // 意味着整条链路无人值守 —— 子代理的权限确认会冒泡到父 UI,同样必须
    // 自动放行,否则 goal 回合里 spawn 的子代理照样弹窗。
    return is_active(session_manager_->current_parent_session_id());
}

void AgentLoop::notify_goal_objective_updated() {
    if (!busy_.load()) return;
    pending_goal_objective_steering_.store(true);
}

void AgentLoop::stop_active_goal_after_turn_error(const ProviderErrorInfo& info) {
    if (!session_manager_) return;
    const std::string sid = session_manager_->current_session_id();
    ThreadGoalStore* store = session_manager_->existing_goal_store();
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
    account_goal_usage(0, false);
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
    if (updated.has_value()) emit_goal_updated(*updated);
    dispatch_message("system",
        usage_limited
            ? "[Goal] Provider usage limit hit; goal marked usage_limited and automatic continuation stopped. Use /goal resume to continue later."
            : "[Goal] Turn ended with an error; goal marked blocked and automatic continuation stopped. Use /goal resume to retry.",
        false, make_system_notice_metadata(usage_limited ? "goal_usage_limited" : "goal_blocked"));
    LOG_WARN("[goal] stopped active goal after turn error: status=" +
             to_string(next) + " provider_status_code=" +
             std::to_string(info.status_code));
}

void AgentLoop::maybe_inject_goal_steering() {
    const bool budget = pending_goal_budget_limit_steering_.exchange(false);
    const bool objective = pending_goal_objective_steering_.exchange(false);
    if (!budget && !objective) return;
    if (!session_manager_) return;
    const std::string sid = session_manager_->current_session_id();
    ThreadGoalStore* store = session_manager_->existing_goal_store();
    if (!store || sid.empty()) return;
    auto goal = store->get_thread_goal(sid);
    if (!goal.has_value()) return;

    auto append_hidden = [this](const std::string& text) {
        ChatMessage msg;
        msg.role = "user";
        msg.content = text;
        msg.metadata = nlohmann::json{{"hidden_goal_context", true}};
        ensure_user_message_identity(msg);
        messages_.push_back(msg);
        if (session_manager_) session_manager_->on_message(msg);
    };

    if (budget && goal->status == ThreadGoalStatus::BudgetLimited) {
        append_hidden(build_goal_budget_limit_prompt(*goal));
        LOG_INFO("[goal] injected budget_limit steering into active turn");
    }
    if (objective && goal->status == ThreadGoalStatus::Active) {
        append_hidden(build_goal_objective_updated_prompt(*goal));
        LOG_INFO("[goal] injected objective_updated steering into active turn");
    }
}

} // namespace acecode
