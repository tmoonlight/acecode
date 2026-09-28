#include "agent/turn/turn_context.hpp"
#include "agent/tool_exec/tool_context_factory.hpp"
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
#include "session/ask_user_question_prompter.hpp"
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
#include <stdexcept>

namespace acecode {

using agent::detail::kDefaultNoModelConfiguredPrompt;

namespace {
AgentLoopServices legacy_services(AgentLoop::ProviderAccessor provider, ToolExecutor& tools,
    AgentCallbacks callbacks, PermissionManager& permissions) {
    AgentLoopServices services{tools, permissions};
    services.provider = std::move(provider);
    services.callbacks = std::move(callbacks);
    return services;
}
AgentLoopOptions legacy_options(std::string cwd) {
    AgentLoopOptions options;
    options.cwd = std::move(cwd);
    return options;
}
}
AgentLoop::AgentLoop(ProviderAccessor provider, ToolExecutor& tools,
    AgentCallbacks callbacks, const std::string& cwd, PermissionManager& permissions)
    : AgentLoop(legacy_services(std::move(provider), tools, std::move(callbacks), permissions),
                legacy_options(cwd)) {
    legacy_auto_started_ = true;
    start();
}

AgentLoop::AgentLoop(AgentLoopServices services, AgentLoopOptions options)
    : callbacks_(std::move(services.callbacks))
    , turn_outcome_(std::make_unique<agent::TurnOutcomeRecord>())
    , context_window_(options.context_window)
    , task_suggestion_compact_threshold_(options.task_suggestion_compact_threshold)
    , provider_accessor_(std::move(services.provider))
    , tools_(services.tools)
    , permissions_(services.permissions)
    , session_manager_(services.session)
    , hook_manager_(services.hooks)
    , runtime_(std::move(services.runtime))
    , skills_snapshot_(std::move(services.skills))
    , expert_snapshot_(std::move(services.expert))
    , computer_use_release_(runtime_.computer_use_release)
    , no_model_config_prompt_(options.no_model_config_prompt.empty()
          ? kDefaultNoModelConfiguredPrompt : std::move(options.no_model_config_prompt))
    , loop_cfg_(options.config)
    , published_loop_config_(std::make_shared<const AgentLoopConfig>(options.config))
    , history_(std::make_unique<agent::ConversationHistory>(busy_))
    , transcript_(std::make_unique<agent::TranscriptWriter>(
          *history_, events_, callbacks_, *turn_outcome_))
    , task_queue_(std::make_unique<agent::AgentTaskQueue>(busy_))
    , active_turn_gate_(std::make_unique<agent::ActiveTurnGate>(
          busy_, abort_signal_, turn_interrupt_requested_))
    , task_handoff_(std::make_unique<agent::TaskHandoff>(*task_queue_))
    , active_provider_slot_(std::make_unique<agent::ActiveProviderSlot>())
    , boundary_(std::make_unique<agent::WorkspaceBoundary>(options.cwd, permissions_))
    , exec_security_(std::make_unique<agent::SessionExecSecurity>(*boundary_, permissions_, busy_, runtime_))
    , hooks_(std::make_unique<agent::AgentHookBridge>(*boundary_, permissions_,
          provider_accessor_, *transcript_, *history_))
    , tool_hooks_(std::make_unique<agent::ToolHookBridge>(*hooks_))
    , goal_(std::make_unique<agent::GoalRuntime>(*task_queue_, *history_, *transcript_,
          events_, callbacks_, permissions_, busy_, abort_signal_))
    , prompt_cache_(std::make_unique<agent::PromptContextCache>())
    , request_builder_(std::make_unique<agent::ApiRequestBuilder>(tools_, *prompt_cache_))
    , side_questions_(std::make_unique<agent::SideQuestionService>(provider_accessor_))
    , activity_(std::make_unique<agent::ActivityNarrator>(callbacks_))
    , retry_progress_(std::make_unique<agent::RetryProgressReporter>(callbacks_, events_))
    , usage_accountant_(std::make_unique<agent::TurnUsageAccountant>(*goal_, callbacks_, events_, last_api_total_tokens_))
    , model_steps_(std::make_unique<agent::ModelStepRecorder>(tools_, events_))
    , stream_collector_(std::make_unique<agent::ProviderStreamCollector>(
          tools_, callbacks_, events_, *history_, *active_provider_slot_, abort_signal_,
          *activity_, *retry_progress_))
    , compaction_(std::make_unique<agent::CompactionController>(
          *history_, *transcript_, *boundary_, *hooks_, *request_builder_, *active_provider_slot_,
          *retry_progress_, callbacks_, events_, abort_signal_, busy_, last_api_total_tokens_, runtime_))
    , recovery_(std::make_unique<agent::ContextOverflowRecovery>(
          *history_, *transcript_, *compaction_, *retry_progress_, *goal_,
          callbacks_, events_, abort_signal_, runtime_))
{
    request_source_.runtime = runtime_;
    request_source_.skills = skills_snapshot_.get();
    request_source_.expert = expert_snapshot_.get();
    request_source_.expert_member = std::move(options.expert_member_id);
    request_source_.memory = services.memory;
    request_source_.skill_usage = services.skill_usage;
    request_source_.skill_idle_days = options.skill_idle_days;
    request_source_.tool_policy = std::move(options.tool_policy);
    request_source_.loop = std::move(options.loop_policy);
    if (session_manager_)
        trajectory_ = std::make_unique<agent::TrajectoryRecorder>(events_, *history_, *session_manager_);
    set_tool_preamble_config(options.config.tool_preamble);
    if (options.sandbox) set_sandbox_config(*options.sandbox);
    set_audit_sink(std::move(services.audit_sink));
    if (!options.exec_rules_dir_override.empty())
        set_exec_rules_dir_for_tests(options.exec_rules_dir_override);
    if (!options.inherited_write_root.empty())
        set_inherited_write_root(std::move(options.inherited_write_root));
    reload_exec_rules();
}

void AgentLoop::start() {
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    if (stopped_) throw std::logic_error("AgentLoop cannot restart after shutdown");
    if (started_) return;
    worker_thread_ = JoiningThread(&AgentLoop::worker_main, this);
    started_ = true;
}
void AgentLoop::require_before_start(const char* operation) const {
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    if (!started_) return;
    if (!legacy_auto_started_)
        throw std::logic_error(std::string(operation) + " must precede AgentLoop::start");
    if (busy_.load() || processed_task_.load())
        LOG_WARN(std::string(operation) + " changes a legacy loop after work has started");
}
void AgentLoop::set_agent_loop_config(AgentLoopConfig config) {
    std::atomic_store(&published_loop_config_,
        std::make_shared<const AgentLoopConfig>(config));
    // Tool presentation already uses its own synchronized publication point.
    set_tool_preamble_config(config.tool_preamble);
}

AgentLoop::~AgentLoop() {
    shutdown();
}

void AgentLoop::set_cwd(const std::string& new_cwd) {
    agent::ToolContextFactory::switch_cwd(
        *boundary_, *exec_security_, *prompt_cache_, permissions_, session_manager_, new_cwd);
}

ResolvedQuestionPolicy AgentLoop::resolved_question_policy() const {
    return resolve_question_policy(*std::atomic_load(&published_loop_config_));
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
    {
        std::lock_guard<std::mutex> lock(lifecycle_mu_);
        stopped_ = true;
    }
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
    require_before_start("set_permission_prompter");
    prompter_ = std::move(p);
}

void AgentLoop::set_ask_question_prompter(std::unique_ptr<AskUserQuestionPrompter> p) {
    require_before_start("set_ask_question_prompter");
    ask_prompter_ = std::move(p);
}

void AgentLoop::set_callbacks(AgentCallbacks cb) {
    callbacks_.publish(std::move(cb));
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
