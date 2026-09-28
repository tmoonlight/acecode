#pragma once

#include "agent/request/request_context_source.hpp"

#include "utils/abort_signal.hpp"
#include "utils/joining_thread.hpp"

#include "agent/callbacks_slot.hpp"
#include "agent/agent_loop_services.hpp"
#include "agent/control/control_receipt.hpp"
#include "agent/turn/turn_types.hpp"

#include "llm/llm_provider.hpp"
#include "tool/tool_executor.hpp"
#include "permissions/permissions.hpp"
#include "permissions/path_validator.hpp"
#include "session/event_dispatcher.hpp"
#include "agent/side_question/side_chat.hpp"
#include "config/config.hpp"
#include "sandbox/exec_permission.hpp"
#include "sandbox/sandbox_denial.hpp"
#include "sandbox/sandbox_runtime.hpp"
#include "security/audit_log.hpp"

#include <vector>
#include <string>
#include <functional>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <optional>
#include <utility>
#include <limits>
#include <memory>
#include <chrono>

namespace acecode {

class SessionManager;
class SkillUsageStore;
class PermissionPrompter;
class AskUserQuestionPrompter;
struct ThreadGoal;
struct TurnSteerResult;
struct HookCommonPayloadFields;
struct HookAggregateOutcome;
class SkillRegistry;
class MemoryRegistry;
class HookManager;
struct MemoryConfig;
struct ProjectInstructionsConfig;
struct ExpertDefinition;
struct CompactResult;
struct SystemPromptModelState;
struct SystemPromptWorkspaceFolders;
class AgentLoopDoomGuard;

namespace agent { class TurnFinalizer; struct TurnContext; struct ToolCallOutcome; struct ToolBatchOutcome; struct ToolBatchState; struct DeferredTaskCompleteEnd; class ContextOverflowRecovery; struct RequestRecoveryState; class CompactionController; struct CompactionInputs; class ProviderStreamCollector; struct TurnUsageRecord; class TurnUsageAccountant; class ModelStepRecorder; struct RequestContextOptions; class ApiRequestBuilder; class PromptContextCache; class ActivityNarrator; class RetryProgressReporter; class SideQuestionService; class ActiveProviderSlot; class SynchronizedDoomGuard; class AgentTaskQueue; class ActiveTurnGate; class TaskHandoff; class GoalRuntime; class AgentHookBridge; class ToolHookBridge; class WorkspaceBoundary; class SessionExecSecurity; class ConversationHistory; class TranscriptWriter; class TrajectoryRecorder; class TurnOutcomeRecord; }

class AgentLoop {
public:
    // Snapshot the current provider at each turn; accessor synchronizes publication.
    using ProviderAccessor = std::function<std::shared_ptr<LlmProvider>()>;

    AgentLoop(AgentLoopServices services, AgentLoopOptions options);
    ~AgentLoop();
    // Install both prompters before start; construction never launches work.
    void start();

    void set_callbacks(AgentCallbacks cb);

    // Submit a user message. Non-blocking: enqueues the message and returns immediately.
    // The internal worker thread will process it.
    void submit(const std::string& user_message);

    // display_text preserves unexpanded user text; empty falls back to prompt.
    void submit(const std::string& prompt, const std::string& display_text);

    // Submit structured user input containing text plus optional attachment or
    // context parts. Existing text-only submit overloads delegate here.
    void submit(const UserInput& input);

    // Retry the trailing user or the last user of an explicitly aborted turn
    // while idle. Reuses stored input without adding adjacent user messages.
    bool retry_last_user_message(const std::string& expected_user_message_id,
                                 std::string& error);

    // !cmd runs on the same worker and persists a shell-context user entry.
    void submit_shell(std::string command);

    // Queue a manual `/compact` control task on the same worker as chat/tool
    // turns so transcript mutation cannot race an active model run.
    void submit_compact();

    // Queue a runtime-context update on the same worker as chat turns. The
    // callback runs between already-queued and subsequently-queued turns, so an
    // in-flight turn keeps its current context while the next turn sees the
    // update.
    ControlEnqueueReceipt enqueue_control(std::function<bool()> control);

    // Run an external model-state mutation only when no work is active or
    // queued. The callback holds the queue gate; it must not submit work, wait
    // for the worker, or acquire ActiveTurnGate. False means it was not run.
    bool try_run_idle_control(const std::function<void()>& control);

    // Emit a visible system message without adding it to LLM history. Used by
    // daemon-owned builtin commands for TUI-like progress and fallback output.
    void emit_system_message(const std::string& content,
                             nlohmann::json metadata = nlohmann::json::object());
    void emit_transcript_system_message(const std::string& content,
                                        nlohmann::json metadata = nlohmann::json::object());

    // Append a single user-role entry to messages_ representing an already-run
    // shell command and its captured output. Used both by the shell worker
    // branch and by --resume to rehydrate LLM context from persisted session
    // messages (`!cmd` user + tool_result pair).
    void inject_shell_turn(const std::string& cmd,
                           const std::string& stdout_text,
                           const std::string& stderr_text,
                           int exit_code);

    // Abort the current inference. Safe to call from any thread.
    void abort();
    void clear_stale_abort_request();

    // Signal the worker thread to exit and wait for it to finish.
    void shutdown();

    // Returns true if abort has been requested. Useful for confirm callbacks.
    bool is_aborting() const { return abort_signal_.raw().load(); }

    // Returns true while the worker is processing a submitted turn.
    bool is_busy() const { return busy_.load(); }

    // Migration must also wait for submitted work not yet picked up by the worker.
    bool has_pending_work();
    bool has_queued_user_work();
    bool submit_task_suggestion_input(const UserInput& input,
                                      const std::string& suggestion_id);
    bool has_task_suggestion_input(const std::string& suggestion_id);
    bool try_start_side_task(const std::function<bool()>& accept_target_input,
                              std::string* error = nullptr);
    // Called from this loop's worker control boundary. Submission of the
    // successor's first input is serialized with incoming source input. The
    // callback may only submit to the target; it must not call this loop.
    bool complete_task_handoff(const std::string& target_session_id,
                               const std::function<bool()>& accept_target_input,
                               std::string* error = nullptr);

    // Append input to the active regular turn. The expected id check and FIFO
    // append happen under one lock, matching Codex turn/steer race semantics.
    TurnSteerResult steer_input(const std::string& expected_turn_id,
                                const UserInput& input);
    // Atomically promise a new high-priority user turn and abort the matching
    // active turn. Unlike steer_input(), this does not wait for the current
    // provider response to reach its next model boundary.
    TurnSteerResult interrupt_turn(const std::string& expected_turn_id,
                                   const UserInput& input);
    // Under ActiveTurnGate, finish a pending question and append same-turn input.
    // No abort/new turn; a stale request returns NoPendingQuestion.
    TurnSteerResult interject_question(const std::string& request_id,
                                       const UserInput& input,
                                       const std::string& expected_turn_id = {});
    std::string active_turn_id() const;

    // Legacy cancel alias
    void cancel() { abort(); }

    // Clear all messages (for /clear command)
    void clear_messages();

    // Restore through the history's idle boundary; no mutable vector escapes.
    void push_message(const ChatMessage& msg);
    const std::vector<ChatMessage>& messages() const;
    void history_on_worker(const std::function<void(agent::ConversationHistory&)>& operation);

    // Safe detached prompt snapshot; callers never read active worker history.
    std::vector<ChatMessage> side_question_context_snapshot() const;
    // Publish a safe baseline snapshot before the first main provider request.
    // SessionRegistry calls this only while the loop is idle, after initial
    // configuration or restored history has been installed.
    void prime_side_question_context();
    SideQuestionResult ask_side_question(const std::string& question);
    SideChatResult stream_side_chat(const std::string& question,
                                    const std::vector<SideChatMessage>& history,
                                    SideChatCancellation& cancellation,
                                    const SideChatStreamCallback& callback);
    using SideQuestionCallback =
        std::function<void(SideQuestionResult)>;
    // Runs the detached provider call without blocking the TUI thread. Worker
    // lifetime is owned by AgentLoop and callbacks are suppressed on shutdown.
    bool ask_side_question_async(std::string question,
                                 SideQuestionCallback callback);

    std::string cwd() const;

    // Changes execution cwd and path validation, retaining session storage cwd.
    // Call from the tool worker or while idle.
    void set_cwd(const std::string& new_cwd);
    // 重读会话所属项目 workspace.json 里的附加文件夹(「编辑项目」保存的
    // extra_folders)。每回合开始调一次,保存后已打开的会话下一轮即生效。
    void refresh_workspace_folders();
    // 系统提示列出的附加文件夹(已过滤掉磁盘上不存在的)。
    std::vector<std::string> workspace_extra_folders() const;
    // Extra writable roots exclude overlap with an inherited worktree/LOOP boundary.
    std::vector<std::string> writable_workspace_folders() const;
    void set_sandbox_config(const SandboxConfig& config);
    void set_exec_rules(sandbox::ExecRules rules);
    // 测试用:把全局规则目录(默认 `<data_dir>/rules`)指到临时目录,让
    // 「批准并记住」的写回不碰真实用户数据;同时影响 reload_exec_rules()。
    void set_exec_rules_dir_for_tests(const std::string& dir);
    void set_sandbox_availability_for_tests(std::optional<bool> value);
    std::string sandbox_command(const std::string& args);
    // 重新读取全局 / 项目规则文件(设置页改了托管规则文件之后由 daemon 触发)。
    void refresh_exec_rules() { reload_exec_rules(); }
    // 安全审计接收器(openspec add-security-center D1):审批门每个「决定已作出」
    // 的分支调一次。默认落到进程级 security::audit_log();单测注入 lambda 收集。
    // 只应在会话未运行时设置。
    void set_audit_sink(security::AuditSink sink);

    // Set only before work; clock/release callbacks are captured for a whole turn.
    using SteadyClockFn = std::function<std::chrono::steady_clock::time_point()>;
    using ComputerUseReleaseFn = std::function<void(const std::string& session_id)>;
    void set_progress_clock_for_tests(SteadyClockFn clock) {
        progress_clock_ = std::move(clock);
    }
    void set_computer_use_release_for_tests(ComputerUseReleaseFn release) {
        computer_use_release_ = std::move(release);
    }

    void set_context_window(int cw) {
        context_window_.store(cw, std::memory_order_relaxed);
    }
    int context_window() const {
        return context_window_.load(std::memory_order_relaxed);
    }
    void set_no_model_config_prompt(std::string prompt) {
        no_model_config_prompt_ = std::move(prompt);
    }

    // Atomically publish policy for the next worker task.
    void set_agent_loop_config(AgentLoopConfig cfg);

    // 工具前言(add-tool-preamble)。配置可在设置页动态改,所以单独一把锁、
    // 每次用时取快照。
    void set_tool_preamble_config(const ToolPreambleConfig& cfg);
    ToolPreambleConfig tool_preamble_config() const;

    void set_task_suggestion_compact_threshold(int threshold) {
        task_suggestion_compact_threshold_.store(
            threshold > 0 ? threshold : 0, std::memory_order_relaxed);
    }

    // Per-session policy used only by daemon-owned LOOP runs. It is installed
    // before the first submit and may be updated once worktree creation adds
    // final branch/path context.
    void set_loop_execution_policy(LoopExecutionPolicy policy);
    LoopExecutionPolicy loop_execution_policy() const {
        std::lock_guard<std::mutex> lock(request_source_mu_);
        return request_source_.loop;
    }

    // Write boundary priority: active worktree, LOOP cwd, inherited parent root.
    // Read tools are unrestricted; dangerous mode explicitly bypasses write checks.
    std::string write_root() const;
    void set_inherited_write_root(std::string root);

    // Latest completed turn outcome, reset at the next turn's start.
    bool last_turn_failed() const;
    std::string last_turn_error() const;
    ResolvedQuestionPolicy resolved_question_policy() const;

    void dispatch_session_start_hook(const std::string& source);
    void dispatch_session_title_changed_hook(const std::string& title,
                                             const std::string& source,
                                             const std::string& title_source);
    void restore_goal_runtime();
    void publish_current_goal_state();
    void maybe_continue_goal();

    // Goal 无人值守模式:当前会话(或子代理的父会话)存在 Active goal 且不在
    // Plan mode 时为 true。工具权限确认自动放行;
    // AskUserQuestion 正常弹 UI，30 秒未回答则自动采纳推荐项。
    bool goal_unattended_active();

    // Active turns consume objective changes at the next model boundary.
    void notify_goal_objective_updated();

    // Called by the serialized control publication path. Each turn retains
    // its previous immutable capability snapshot until completion.
    void publish_skill_snapshot(std::shared_ptr<const SkillRegistry> skills);
    void publish_expert_snapshot(std::shared_ptr<const ExpertDefinition> expert,
        std::shared_ptr<const SkillRegistry> skills, ToolCapabilityPolicy policy,
        std::string member_id = {});
    std::set<std::string> dormant_skill_names() const;
    void set_tool_capability_policy(ToolCapabilityPolicy policy) {
        std::lock_guard<std::mutex> lock(request_source_mu_);
        request_source_.tool_policy = std::move(policy);
    }
    ToolCapabilityPolicy tool_capability_policy() const {
        std::lock_guard<std::mutex> lock(request_source_mu_);
        return request_source_.tool_policy;
    }
    // 外部 git 状态变更(如 Web UI checkout 分支)后标记快照过期。线程安全:
    // 任意线程可调;worker 在下一次模型请求前消费标记并重采。正在跑的 turn
    // 继续用旧快照 —— 快照本身声明为 point-in-time,一回合的陈旧无害。
    void invalidate_git_snapshot();

    // ---- 事件流(Section 7 SessionClient)----
    // 老的 AgentCallbacks 路径**完全不动**:TUI 仍然用 callbacks。
    // SessionClient 走 events_,daemon HTTP/WebSocket handler 在 subscribe 上
    // 拿事件流。两者并行,不互相影响。
    EventDispatcher& events() { return events_; }

    // Both event-backed prompters must be installed before start().
    void set_permission_prompter(std::unique_ptr<PermissionPrompter> p);

    // Owned by this loop; SessionEntry may retain only a borrowed alias.
    void set_ask_question_prompter(std::unique_ptr<AskUserQuestionPrompter> prompter);

    // TUI transport alternative to the daemon prompter. Bind before work starts.
    using AskQuestionChannel = std::function<nlohmann::json(
        const nlohmann::json& questions_payload,
        const std::atomic<bool>* abort_flag,
        int timeout_seconds,
        const std::string& origin_label)>;
    void set_ask_question_channel(AskQuestionChannel channel) {
        ask_channel_ = std::move(channel);
    }

private:
    void worker_main();
    void recover_worker_task_error(const char* detail, bool chat_task);
    std::unique_ptr<agent::TurnFinalizer> make_turn_finalizer();
    void run_agent_with_input(const UserInput& input, bool hidden_goal_context = false,
                              const ChatMessage* retry_message = nullptr);
    std::optional<ChatMessage> retryable_user_message(const std::string& id) const;
    void run_shell(std::string command);
    void run_compact();
    void emit_goal_updated(const ThreadGoal& goal);
    void wake_active_provider_retry();
    agent::CompactionInputs compaction_inputs() const;
    std::vector<ChatMessage> build_compaction_initial_context() const;
    agent::RequestContextOptions request_context_options(
        const std::shared_ptr<LlmProvider>& provider, bool swarm_mode = false) const;
    void publish_side_question_context(const std::vector<ChatMessage>& messages);
    void require_before_start(const char* operation) const;
    void reload_exec_rules();
    void release_computer_use_session(const std::string& session_id) const;
    using WorkerTask = agent::WorkerTask;

    // Roots outlive all collaborators and prompters. Worker is always last.
    AbortSignal abort_signal_;
    std::atomic<bool> turn_interrupt_requested_{false};
    std::atomic<bool> busy_{false};
    EventDispatcher events_;
    CallbacksSlot callbacks_;
    std::unique_ptr<agent::TurnOutcomeRecord> turn_outcome_;
    std::atomic<int> context_window_{128000};
    std::atomic<int> last_api_total_tokens_{0};
    std::atomic<int> task_suggestion_compact_threshold_{3};
    mutable std::mutex lifecycle_mu_;
    bool started_ = false;
    bool stopped_ = false;

    // Required references and optional borrowed dependencies are fixed at init.
    ProviderAccessor provider_accessor_;
    ToolExecutor& tools_;
    PermissionManager& permissions_;
    SessionManager* const session_manager_; // Nullable borrowed; fixed at construction.
    HookManager* const hook_manager_; // Nullable borrowed; fixed at construction.
    AgentRuntimeEnv runtime_;
    const PromptConfigProvider prompt_config_provider_;
    SteadyClockFn progress_clock_;
    ComputerUseReleaseFn computer_use_release_;
    std::string no_model_config_prompt_;
    AgentLoopConfig loop_cfg_; // Worker task snapshot, never written by a publisher.
    std::shared_ptr<const AgentLoopConfig> published_loop_config_;
    mutable std::mutex request_source_mu_;
    agent::RequestContextSource request_source_;
    agent::RequestContextSource capture_request_source() const;

    // Assembly DAG: history/outcome -> transcript -> queue/gate -> boundary ->
    // security/hooks -> goal/requests -> model/compaction/recovery -> turn scope.
    std::unique_ptr<agent::ConversationHistory> history_;
    std::unique_ptr<agent::TranscriptWriter> transcript_;
    std::unique_ptr<agent::TrajectoryRecorder> trajectory_;
    std::unique_ptr<agent::AgentTaskQueue> task_queue_;
    std::unique_ptr<agent::ActiveTurnGate> active_turn_gate_;
    std::unique_ptr<agent::TaskHandoff> task_handoff_;
    std::unique_ptr<agent::ActiveProviderSlot> active_provider_slot_;
    std::unique_ptr<agent::WorkspaceBoundary> boundary_;
    std::unique_ptr<agent::SessionExecSecurity> exec_security_;
    std::unique_ptr<agent::AgentHookBridge> hooks_;
    std::unique_ptr<agent::ToolHookBridge> tool_hooks_;
    std::unique_ptr<agent::GoalRuntime> goal_;
    std::unique_ptr<agent::PromptContextCache> prompt_cache_;
    std::unique_ptr<agent::ApiRequestBuilder> request_builder_;
    std::unique_ptr<agent::SideQuestionService> side_questions_;
    std::unique_ptr<agent::ActivityNarrator> activity_;
    std::unique_ptr<agent::RetryProgressReporter> retry_progress_;
    std::unique_ptr<agent::TurnUsageAccountant> usage_accountant_;
    std::unique_ptr<agent::ModelStepRecorder> model_steps_;
    std::unique_ptr<agent::ProviderStreamCollector> stream_collector_;
    std::unique_ptr<agent::CompactionController> compaction_;
    std::unique_ptr<agent::ContextOverflowRecovery> recovery_;
    std::unique_ptr<agent::TurnContext> turn_context_;

    // Both prompters depend on events_; neither may outlive the loop.
    std::unique_ptr<PermissionPrompter> prompter_;
    std::unique_ptr<AskUserQuestionPrompter> ask_prompter_;
    AskQuestionChannel ask_channel_;
    JoiningThread worker_thread_;
};

} // namespace acecode
