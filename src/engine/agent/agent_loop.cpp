#include "agent/agent_loop.hpp"
#include "agent/recovery/context_overflow_recovery.hpp"
#include "agent/compaction/compaction_controller.hpp"
#include "agent/model_step/provider_stream_collector.hpp"
#include "agent/model_step/turn_usage_accountant.hpp"
#include "agent/model_step/model_step_recorder.hpp"
#include "agent/request/prompt_context_cache.hpp"
#include "agent/request/api_request_builder.hpp"
#include "agent/progress/activity_narrator.hpp"
#include "agent/progress/retry_progress.hpp"
#include "agent/side_question/side_question_service.hpp"
#include "agent/hook_bridge/tool_hook_bridge.hpp"
#include "agent/hook_bridge/agent_hook_bridge.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "agent/approval/session_exec_security.hpp"
#include "agent/boundary/workspace_boundary.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/transcript/trajectory_recorder.hpp"
#include "agent/turn/turn_outcome.hpp"
#include "agent/control/task_handoff.hpp"
#include "agent/turn/active_turn_gate.hpp"
#include "agent/worker/agent_task_queue.hpp"
#include "agent/model_step/active_provider_slot.hpp"
#include "agent/detail/agent_payloads.hpp"
#include "agent/guards/doom_guard.hpp"
#include "computer_use/runtime.hpp"
#include "hooks/hook_manager.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_manager.hpp"
#include "session/token_tracker.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>
#include <thread>

namespace acecode {

using agent::detail::kDefaultNoModelConfiguredPrompt;

AgentLoop::AgentLoop(ProviderAccessor provider_accessor, ToolExecutor& tools,
                     AgentCallbacks callbacks, const std::string& cwd,
                     PermissionManager& permissions)
    : provider_accessor_(std::move(provider_accessor))
    , tools_(tools)
    , callbacks_(std::move(callbacks))
    , history_(std::make_unique<agent::ConversationHistory>(busy_))
    , turn_outcome_(std::make_unique<agent::TurnOutcomeRecord>())
    , transcript_(std::make_unique<agent::TranscriptWriter>(
          *history_, events_, callbacks_, *turn_outcome_))
    , active_provider_slot_(std::make_unique<agent::ActiveProviderSlot>())
    , boundary_(std::make_unique<agent::WorkspaceBoundary>(cwd, permissions))
    , exec_security_(std::make_unique<agent::SessionExecSecurity>(*boundary_, permissions, busy_))
    , permissions_(permissions)
    , no_model_config_prompt_(kDefaultNoModelConfiguredPrompt)
    , turn_usage_(std::make_unique<agent::TurnUsageRecord>())
    , recovery_state_(std::make_unique<agent::RequestRecoveryState>())
    , task_queue_(std::make_unique<agent::AgentTaskQueue>(busy_))
    , active_turn_gate_(std::make_unique<agent::ActiveTurnGate>(
          busy_, abort_signal_, turn_interrupt_requested_))
    , task_handoff_(std::make_unique<agent::TaskHandoff>(*task_queue_))
    , hooks_(std::make_unique<agent::AgentHookBridge>(*boundary_, permissions,
          provider_accessor_, *transcript_, *history_))
    , tool_hooks_(std::make_unique<agent::ToolHookBridge>(*hooks_))
    , goal_(std::make_unique<agent::GoalRuntime>(*task_queue_, *history_, *transcript_,
          events_, callbacks_, permissions, busy_, abort_signal_))
    , prompt_cache_(std::make_unique<agent::PromptContextCache>())
    , request_builder_(std::make_unique<agent::ApiRequestBuilder>(tools_, *prompt_cache_))
    , side_questions_(std::make_unique<agent::SideQuestionService>(provider_accessor_))
    , activity_(std::make_unique<agent::ActivityNarrator>(callbacks_))
    , retry_progress_(std::make_unique<agent::RetryProgressReporter>(callbacks_, events_))
    , usage_accountant_(std::make_unique<agent::TurnUsageAccountant>(*goal_, callbacks_, events_, last_api_total_tokens_))
    , model_steps_(std::make_unique<agent::ModelStepRecorder>(tools_, events_))
    , stream_collector_(std::make_unique<agent::ProviderStreamCollector>(
          tools_, callbacks_, events_, *history_, *active_provider_slot_, abort_signal_,
          *activity_, *retry_progress_, *usage_accountant_, *model_steps_))
    , compaction_(std::make_unique<agent::CompactionController>(
          *history_, *transcript_, *boundary_, *hooks_, *request_builder_, *active_provider_slot_,
          *retry_progress_, callbacks_, events_, abort_signal_, busy_, last_api_total_tokens_))
    , recovery_(std::make_unique<agent::ContextOverflowRecovery>(
          *history_, *transcript_, *compaction_, *retry_progress_, *goal_,
          callbacks_, events_, abort_signal_))
{
    reload_exec_rules();
    worker_thread_ = JoiningThread(&AgentLoop::worker_main, this);
}

AgentLoop::~AgentLoop() {
    shutdown();
}

void AgentLoop::set_cwd(const std::string& new_cwd) {
    boundary_->set_cwd(new_cwd);
    // cwd 变了(EnterWorktree/ExitWorktree),旧 gitStatus 快照作废,
    // 下一次模型请求按新 cwd 重采(openspec add-git-context)。
    prompt_cache_->reset_on_cwd_change();
    permissions_.clear_session_allows();
    exec_security_->runtime().clear_session_grants();
    exec_security_->set_feedback(std::nullopt);
    reload_exec_rules();
    // 进出 worktree 会改变写边界,可写附加文件夹随之重算。
    exec_security_->runtime().set_workspace_writable_roots(writable_workspace_folders());
}

ResolvedQuestionPolicy AgentLoop::resolved_question_policy() const {
    return resolve_question_policy(loop_cfg_);
}

void AgentLoop::abort() {
    abort_signal_.request();
    if (session_manager_) release_computer_use_session(session_manager_->current_session_id());
    wake_active_provider_retry();
}

// computer-use 会话租约的唯一释放出口(abort / 回合收尾 / DesktopTurnLease 析构都走
// 这里)。默认原样调用 computer_use::release_session;P0-11 的表征测试注入 fake
// 记录 owner 与释放次数,不启动真实桌面 helper。
void AgentLoop::release_computer_use_session(const std::string& session_id) const {
    if (computer_use_release_) {
        computer_use_release_(session_id);
        return;
    }
    computer_use::release_session(session_id);
}

void AgentLoop::wake_active_provider_retry() {
    active_provider_slot_->wake();
}

void AgentLoop::clear_stale_abort_request() {
    if (!busy_.load()) {
        abort_signal_.clear();
    }
}

void AgentLoop::shutdown() {
    side_questions_->stop_requests();
    task_queue_->request_shutdown();
    abort_signal_.request();
    wake_active_provider_retry();
    task_queue_->notify();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    side_questions_->join();
}

void AgentLoop::set_permission_prompter(std::unique_ptr<PermissionPrompter> p) {
    prompter_ = std::move(p);
}

void AgentLoop::set_callbacks(AgentCallbacks cb) {
    callbacks_ = std::move(cb);
}

bool AgentLoop::has_pending_work() {
    return task_queue_->has_pending_work();
}

bool AgentLoop::has_queued_user_work() {
    return task_queue_->has_user_work();
}

bool AgentLoop::has_task_suggestion_input(const std::string& suggestion_id) {
    return task_queue_->has_suggestion(suggestion_id);
}

} // namespace acecode
