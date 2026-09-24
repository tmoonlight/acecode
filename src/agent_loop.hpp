#pragma once

#include "provider/llm_provider.hpp"
#include "tool/tool_executor.hpp"
#include "permissions.hpp"
#include "utils/path_validator.hpp"
#include "utils/token_tracker.hpp"
#include "session/session_manager.hpp"
#include "session/event_dispatcher.hpp"
#include "session/side_chat.hpp"
#include "session/permission_prompter.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "config/config.hpp"
#include "tool_preamble/tool_preamble.hpp"
#include "hooks/hook_runtime.hpp"
#include "skills/skill_usage_store.hpp"
#include "pa/pa_overflow_rescue.hpp"
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
#include <queue>
#include <map>
#include <set>
#include <optional>
#include <utility>
#include <limits>
#include <memory>
#include <chrono>

namespace acecode {

struct LoopExecutionPolicy {
    bool active = false;
    std::string system_context;
};

// Authoritative receipt for a control task inserted into the AgentLoop worker
// queue. queued_behind_turn is computed while holding the same mutex that
// orders chat/control tasks, so a chat that has been submitted but has not yet
// flipped busy=true is still observed. Completion and success are separate: a
// callback that ran but could not commit its state must not be reported as
// applied.
struct ControlExecutionState {
    mutable std::mutex mu;
    std::condition_variable cv;
    bool completed = false;
    bool succeeded = false;
};

struct ControlEnqueueReceipt {
    std::uint64_t sequence = 0;
    bool accepted = false;
    bool queued_behind_turn = false;
    std::shared_ptr<ControlExecutionState> execution;

    bool completed() const {
        if (!execution) return false;
        std::lock_guard<std::mutex> lock(execution->mu);
        return execution->completed;
    }

    bool succeeded() const {
        if (!execution) return false;
        std::lock_guard<std::mutex> lock(execution->mu);
        return execution->completed && execution->succeeded;
    }

    bool applied() const {
        return succeeded();
    }

    bool wait_for_completion(std::chrono::milliseconds timeout) const {
        if (!execution) return false;
        std::unique_lock<std::mutex> lock(execution->mu);
        return execution->cv.wait_for(
            lock, timeout, [&] { return execution->completed; });
    }
};

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

// Callbacks for the TUI to observe agent loop events
struct AgentCallbacks {
    // Called when a new message is added to the conversation
    std::function<void(const std::string& role, const std::string& content, bool is_tool)> on_message;

    // Metadata-preserving observer for persisted transcript-only messages.
    // When installed, it receives those messages instead of the legacy
    // three-field on_message callback so UI grouping can use stable metadata.
    std::function<void(const ChatMessage& message)> on_transcript_message;

    // Called after each tool execution with the structured ToolResult so the
    // TUI can render a summary row. Fires in addition to on_message (not in
    // place of it) so consumers that only care about the text stream continue
    // to work unchanged. Receives the tool_call message too so the TUI can
    // correlate summaries with their call rows.
    std::function<void(const ChatMessage& call_msg,
                       const std::string& tool_name,
                       const ToolResult& result)> on_tool_result;

    // Called when the agent starts/stops processing
    std::function<void(bool busy)> on_busy_changed;

    // Called once for a submitted agent turn immediately before its terminal
    // busy=false callback. Values match persisted turn timing status:
    // "completed", "error", or "aborted". Compact/background busy cycles do
    // not invoke this hook.
    std::function<void(const std::string& status)> on_turn_finished;

    // Called to request user confirmation for a tool call.
    // Returns: Allow, Deny, or AlwaysAllow
    std::function<PermissionResult(const std::string& tool_name, const std::string& arguments)> on_tool_confirm;

    // Called for each streaming delta token (real-time TUI update)
    std::function<void(const std::string& token)> on_delta;

    // Called when token usage data is received from the provider
    std::function<void(const TokenUsage& usage)> on_usage;

    // Called when the current thread goal status changes. Empty string means
    // no goal is active for the current session.
    std::function<void(const std::string& status)> on_goal_status;

    // Called when TodoWrite publishes or reads the current visible checklist.
    // The payload shape matches the todo_updated session event.
    std::function<void(const nlohmann::json& payload)> on_todo_updated;

    // 工具前言(add-tool-preamble):模型步的标题定下来之后、工具执行之前回调
    // (title, source ∈ prompt|reasoning|sidecar)。TUI 据此在该批次的 tool_call
    // 行之前插入标题伪行;prompt 来源时把刚流完的那条 assistant 正文原地改成
    // 标题行,避免同一句话显示两遍。
    std::function<void(const std::string& title, const std::string& source)> on_tool_preamble;

    // reasoning 模式:推理摘要的加粗标题在流式期间就绪时回调,TUI 用它替换
    // 等待动画里的随机短语("Thinking" → "Reading registry sections")。
    std::function<void(const std::string& title)> on_thinking_title;

    // Legacy display observer for replacement-style transcript updates. Normal
    // compact success appends marker messages and no longer calls this hook.
    std::function<void(const std::vector<ChatMessage>& messages,
                       const CompactResult& result)> on_transcript_replace;

    // Called before a provider retry replays the current model request.
    // Consumers should clear provisional live assistant output from the failed
    // stream attempt; persisted history is unchanged.
    std::function<void()> on_stream_retry_reset;

    // Presentation-only retry lifecycle; neither callback appends transcript
    // messages.
    std::function<void(const ProviderErrorInfo&)> on_model_retry;
    std::function<void()> on_model_retry_resume;

    // Called just before a tool begins executing. `command_preview` is a short
    // human-readable summary (e.g. the first 60 chars of a bash command).
    // `preamble` 是工具前言(add-tool-preamble)给这次调用的一句话,空 = 没有。
    std::function<void(const std::string& tool_name,
                       const std::string& command_preview,
                       const std::string& preamble)> on_tool_progress_start;

    // Called from the tool's streaming thread with each cleaned chunk.
    // `tail_snapshot` is the last-5-lines sliding window; `current_partial` is
    // the in-progress line (not yet terminated by \n).
    std::function<void(const std::vector<std::string>& tail_snapshot,
                       const std::string& current_partial,
                       size_t total_bytes,
                       int total_lines)> on_tool_progress_update;

    // Called after the tool returns (or throws). Guaranteed via RAII to fire
    // once for every on_tool_progress_start.
    std::function<void()> on_tool_progress_end;
};

class AgentLoop {
public:
    // provider_accessor: 每轮 turn 开始时调用,返回当前有效的 provider 的
    // shared_ptr 快照。调用方负责在该函数内部加锁保护 main.cpp 的 provider
    // 替换(见 design D4 / 任务 4.6)。这样 worker 即使跨 turn 持有 snapshot
    // 也不会悬空,下一轮再拿最新的。
    using ProviderAccessor = std::function<std::shared_ptr<LlmProvider>()>;

    AgentLoop(ProviderAccessor provider_accessor, ToolExecutor& tools,
              AgentCallbacks callbacks, const std::string& cwd,
              PermissionManager& permissions);
    ~AgentLoop();

    void set_callbacks(AgentCallbacks cb);

    // Submit a user message. Non-blocking: enqueues the message and returns immediately.
    // The internal worker thread will process it.
    void submit(const std::string& user_message);

    // Submit with separate "LLM prompt" vs "UI display" texts. `prompt` is what
    // the model sees in `messages_` (and persisted JSONL);`display_text` is
    // recorded in `user_msg.metadata.display_text` so UI can show the original
    // user input even though the model sees an expanded form (e.g. daemon-side
    // skill command expansion). Empty `display_text` falls back to `prompt`.
    void submit(const std::string& prompt, const std::string& display_text);

    // Submit structured user input containing text plus optional attachment or
    // context parts. Existing text-only submit overloads delegate here.
    void submit(const UserInput& input);

    // Retry the trailing user or the last user of an explicitly aborted turn
    // while idle. Reuses stored input without adding adjacent user messages.
    bool retry_last_user_message(const std::string& expected_user_message_id,
                                 std::string& error);

    // Submit a user-initiated shell command triggered by `!` mode. Non-blocking:
    // enqueues on the same worker so it serialises with LLM turns. The worker
    // invokes BashTool directly (no LLM round-trip), emits tool_call + tool_result
    // UI messages via callbacks, and appends a `<bash-input>/<bash-stdout>/...`
    // user-role entry to messages_ for the next LLM turn.
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
    // for the worker, or acquire active_turn_mu_. False means it was not run.
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
    bool is_aborting() const { return abort_requested_.load(); }

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
    // 提问挂起时的用户插话(daemon 路径):把 request_id 对应的
    // AskUserQuestion 以「用户改为直接输入」收掉,并把 input 作为同回合
    // steering 输入排在该工具结果之后提交 —— 不 abort、不开新回合。
    // 两步在 active_turn_mu_ 下一起完成:worker 要等工具返回后才会
    // drain,所以模型看到的顺序恒为 tool_call → tool_result → user 插话。
    // expected_turn_id 可空;非空时与 steer_input 一样校验。
    // 问题已被回答 / 超时 / 关闭 → NoPendingQuestion,input 不会被提交,
    // 调用方应退回普通发送路径。
    TurnSteerResult interject_question(const std::string& request_id,
                                       const UserInput& input,
                                       const std::string& expected_turn_id = {});
    std::string active_turn_id() const;

    // Legacy cancel alias
    void cancel() { abort(); }

    // Clear all messages (for /clear command)
    void clear_messages() {
        messages_.clear();
        live_transcript_tail_blocked_ = false;
        last_api_total_tokens_.store(0, std::memory_order_relaxed);
        compact_window_initialized_ = false;
        compact_window_number_ = 0;
        compact_first_window_id_.clear();
        compact_current_window_id_.clear();
    }

    // Push a message (for session restore)
    void push_message(const ChatMessage& msg) { messages_.push_back(msg); }

    const std::vector<ChatMessage>& messages() const { return messages_; }
    std::vector<ChatMessage>& messages_mut() { return messages_; }

    // Copy of the latest complete provider-facing prompt built by the worker.
    // `/btw` callers use this instead of reading messages_ from an HTTP thread,
    // which would race the active turn. The snapshot is intentionally detached
    // from transcript/session persistence.
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

    const std::string& cwd() const { return cwd_; }

    // 切换会话工作目录(enter_worktree / exit_worktree / worktree resume 恢复)。
    // 更新 cwd_ 并以新根重建 PathValidator;会话存储位置(SessionManager 的
    // project dir)不动 —— worktree 是同一个项目会话的临时工作区,不是新项目。
    // 只应在工具执行线程(turn 内)或会话未运行时调用。
    void set_cwd(const std::string& new_cwd);
    // 重读会话所属项目 workspace.json 里的附加文件夹(「编辑项目」保存的
    // extra_folders)。每回合开始调一次,保存后已打开的会话下一轮即生效。
    void refresh_workspace_folders();
    // 系统提示列出的附加文件夹(已过滤掉磁盘上不存在的)。
    std::vector<std::string> workspace_extra_folders() const;
    // 真正放行写入的附加文件夹:文件工具的路径校验、bash 写边界守卫与沙箱可写
    // 根都用它。有写边界(worktree / LOOP / 继承)时去掉与主文件夹重叠的项 ——
    // 否则附加一个主仓的上级目录就能绕开 worktree 隔离。
    std::vector<std::string> writable_workspace_folders() const;
    void set_sandbox_config(const SandboxConfig& config);
    void set_exec_rules(sandbox::ExecRules rules) { exec_rules_ = std::move(rules); }
    // 测试用:把全局规则目录(默认 `<data_dir>/rules`)指到临时目录,让
    // 「批准并记住」的写回不碰真实用户数据;同时影响 reload_exec_rules()。
    void set_exec_rules_dir_for_tests(const std::string& dir) {
        exec_rules_dir_override_ = dir;
        reload_exec_rules();
    }
    void set_sandbox_availability_for_tests(std::optional<bool> value) {
        sandbox_runtime_.set_availability_override_for_tests(value);
    }
    std::string sandbox_command(const std::string& args);
    // 重新读取全局 / 项目规则文件(设置页改了托管规则文件之后由 daemon 触发)。
    void refresh_exec_rules() { reload_exec_rules(); }
    // 安全审计接收器(openspec add-security-center D1):审批门每个「决定已作出」
    // 的分支调一次。默认落到进程级 security::audit_log();单测注入 lambda 收集。
    // 只应在会话未运行时设置。
    void set_audit_sink(security::AuditSink sink) { audit_sink_ = std::move(sink); }

    void set_context_window(int cw) {
        context_window_.store(cw, std::memory_order_relaxed);
    }
    int context_window() const {
        return context_window_.load(std::memory_order_relaxed);
    }
    void set_no_model_config_prompt(std::string prompt) {
        no_model_config_prompt_ = std::move(prompt);
    }

    // Install / update the agent-loop termination policy. Called once from
    // main.cpp at startup (and could be called again if config reloads).
    // A fresh-default AgentLoopConfig is used when this setter is never called.
    void set_agent_loop_config(AgentLoopConfig cfg) {
        loop_cfg_ = cfg;
        set_tool_preamble_config(cfg.tool_preamble);
        set_jb_mode(cfg.jb_mode);
    }

    // 开发者模式 JB 开关。设置页可在会话进行中改,读路径只看这个原子值。
    void set_jb_mode(bool enabled) {
        jb_mode_.store(enabled, std::memory_order_relaxed);
    }
    bool jb_mode() const {
        return jb_mode_.load(std::memory_order_relaxed);
    }

    // 工具前言(add-tool-preamble)。配置可在设置页动态改,所以单独一把锁、
    // 每次用时取快照;sidecar 摘要器由入口注入(SessionRegistry / main.cpp),
    // AgentLoop 只知道「给材料、拿标题」,不关心 provider 怎么来的。摘要器在
    // 独立线程里被调用,必须自带超时并可以并发调用。
    void set_tool_preamble_config(const ToolPreambleConfig& cfg);
    ToolPreambleConfig tool_preamble_config() const;
    using ToolPreambleSidecarSummarizer =
        std::function<std::string(const tool_preamble::SidecarSummaryInput& input)>;
    void set_tool_preamble_sidecar_summarizer(ToolPreambleSidecarSummarizer summarizer);

    void set_task_suggestion_compact_threshold(int threshold) {
        task_suggestion_compact_threshold_.store(
            threshold > 0 ? threshold : 0, std::memory_order_relaxed);
    }

    // Per-session policy used only by daemon-owned LOOP runs. It is installed
    // before the first submit and may be updated once worktree creation adds
    // final branch/path context.
    void set_loop_execution_policy(LoopExecutionPolicy policy) {
        loop_execution_policy_ = std::move(policy);
    }
    const LoopExecutionPolicy& loop_execution_policy() const {
        return loop_execution_policy_;
    }

    // 写边界根目录。非空 = 写工具(以及 bash 中可证明的写目标)必须落在该
    // 目录内,与权限模式无关 —— Yolo 不再豁免,只有 dangerous 模式整体放行。
    // 三种来源按优先级取第一个命中:
    //   1. 会话 worktree(含 spawn_subagent 从父会话继承来的 worktree);
    //   2. daemon LOOP 执行策略(边界 = 当前 cwd);
    //   3. spawn_subagent 透传的父会话 write_root(父会话是无 worktree 的
    //      LOOP 会话时靠这条,否则子会话什么都继承不到)。
    // 读工具不受限:父会话读别的 worktree 的记录是合理需求。
    std::string write_root() const;
    void set_inherited_write_root(std::string root) {
        inherited_write_root_ = std::move(root);
    }

    // 上一回合结果:wait_subagent 用它区分"跑完"与"夭折"。回合因 provider
    // 终止错误 / 上下文压缩失败 / 连续空回复耗尽 / max_iterations / hook 拦截
    // 而结束时为 failed,last_turn_error() 带最后一条 error 文案。回合开始时
    // 清零,所以只反映最近一次 submit 的结果。
    bool last_turn_failed() const {
        return last_turn_outcome_.load(std::memory_order_acquire) == kTurnOutcomeError;
    }
    std::string last_turn_error() const;
    ResolvedQuestionPolicy resolved_question_policy() const;

    void set_session_manager(SessionManager* sm);
    void set_hook_manager(HookManager* hm) { hook_manager_ = hm; }

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

    // /goal edit 修改了 active goal 的 objective 时调用。回合运行中则在下一次
    // 模型请求前注入 objective_updated steering(对齐 Codex ext/goal 的
    // inject_active_turn_steering);空闲时为 no-op(下一次 continuation 自然
    // 带新 objective)。
    void notify_goal_objective_updated();

    void set_skill_registry(const SkillRegistry* sr) { skill_registry_ = sr; }
    void set_skill_usage_store(SkillUsageStore* store) { skill_usage_store_ = store; }
    void set_skill_idle_days(int days) { skill_idle_days_ = days; }
    // Names of skills that are dormant (idle past the threshold, not pinned).
    // Returns an empty set when dormancy is disabled or the store is unset.
    std::set<std::string> dormant_skill_names() const;
    void set_memory_registry(const MemoryRegistry* mr) { memory_registry_ = mr; }
    void set_memory_config(const MemoryConfig* cfg) { memory_cfg_ = cfg; }
    void set_project_instructions_config(const ProjectInstructionsConfig* cfg) {
        project_instructions_cfg_ = cfg;
    }
    void set_custom_instructions_config(const CustomInstructionsConfig* cfg) {
        custom_instructions_cfg_ = cfg;
    }
    void set_expert_context(const ExpertDefinition* expert,
                            std::string member_id = {}) {
        expert_ = expert;
        expert_member_id_ = std::move(member_id);
    }
    void set_tool_capability_policy(ToolCapabilityPolicy policy) {
        tool_capability_policy_ = std::move(policy);
    }
    const ToolCapabilityPolicy& tool_capability_policy() const {
        return tool_capability_policy_;
    }
    void set_git_context_config(const GitContextConfig* cfg) {
        git_context_cfg_ = cfg;
    }
    // 外部 git 状态变更(如 Web UI checkout 分支)后标记快照过期。线程安全:
    // 任意线程可调;worker 在下一次模型请求前消费标记并重采。正在跑的 turn
    // 继续用旧快照 —— 快照本身声明为 point-in-time,一回合的陈旧无害。
    void invalidate_git_snapshot() { git_snapshot_stale_.store(true); }

    // ---- 事件流(Section 7 SessionClient)----
    // 老的 AgentCallbacks 路径**完全不动**:TUI 仍然用 callbacks。
    // SessionClient 走 events_,daemon HTTP/WebSocket handler 在 subscribe 上
    // 拿事件流。两者并行,不互相影响。
    EventDispatcher& events() { return events_; }

    // 注入异步 PermissionPrompter(daemon 模式)。不调用此 setter 时,AgentLoop
    // 默认走 callbacks_.on_tool_confirm 同步路径(TUI 模式)。线程安全要求:
    // 不在 worker 跑工具时调用 — 通常 SessionRegistry 创建 AgentLoop 后立刻
    // 调,然后才 submit 第一条消息。
    void set_permission_prompter(std::unique_ptr<PermissionPrompter> p) {
        prompter_ = std::move(p);
    }

    // 注入异步 AskUserQuestionPrompter(daemon 模式)。raw 指针;生命周期由
    // 调用方(典型是 SessionEntry)保证。AgentLoop 在每次工具调用前把它包成
    // ToolContext::ask_user_questions 回调注入。
    void set_ask_question_prompter(AskUserQuestionPrompter* p) {
        ask_prompter_ = p;
    }

    // 注入 TUI 侧的 AskUserQuestion 传输通道。与上面的 prompter 二选一 ——
    // daemon 用 prompter(WS 往返),TUI 用这个(overlay 阻塞等待)。两者
    // 最终都被包成同一个 `ToolContext::ask_user_questions`,因此两端注册的
    // 是同一个 AskUserQuestion 工具工厂,且任何工具都能向用户提问。
    // 参数:questions_payload / abort_flag / timeout_seconds(0 = 无限期)
    //         / origin_label(子代理提问的来源标注)。
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
    bool has_queued_user_work_locked() const;
    void join_side_question_threads();
    void run_agent(const std::string& user_message);
    void run_agent_with_input(const UserInput& input,
                              bool hidden_goal_context = false,
                              const ChatMessage* retry_message = nullptr);
    std::optional<ChatMessage> retryable_user_message(
        const std::string& expected_user_message_id) const;
    // Variant that records `display_text` into the user message's metadata.display_text
    // so UI can show the original input while the LLM sees an expanded `prompt`.
    // When `display_text` is empty, behaves identically to run_agent(prompt).
    void run_agent_with_display(const std::string& prompt,
                                const std::string& display_text,
                                bool hidden_goal_context = false);
    void run_shell(std::string command);
    void run_compact();
    void account_goal_usage(std::int64_t token_delta = 0, bool allow_complete = false);
    void emit_goal_updated(const ThreadGoal& goal);
    void emit_goal_cleared(const std::string& session_id);
    void emit_todo_updated(const nlohmann::json& payload);
    std::string build_goal_context_prompt(const ThreadGoal& goal) const;
    std::string build_goal_budget_limit_prompt(const ThreadGoal& goal) const;
    std::string build_goal_objective_updated_prompt(const ThreadGoal& goal) const;
    // 回合失败(provider 终止错误 / 连续空回复 / provider 缺失)时停止 Active
    // goal:HTTP 429 → usage_limited,其余 → blocked。对齐 Codex ext/goal 的
    // on_turn_error,防止 maybe_continue_goal 对着同一个错误无限重试烧 token。
    void stop_active_goal_after_turn_error(const ProviderErrorInfo& info);
    void set_active_provider_for_retry(
        const std::shared_ptr<LlmProvider>& provider);
    void clear_active_provider_for_retry(
        const std::shared_ptr<LlmProvider>& provider);
    void wake_active_provider_retry();
    // 在每次模型请求前消费 pending steering 标记,把 budget_limit /
    // objective_updated 提示以 hidden_goal_context user 消息注入。
    void maybe_inject_goal_steering();
    void begin_active_turn(const std::string& turn_id);
    void commit_turn_steering_input(UserInput input,
                                    const std::string& turn_id);
    // Worker-only. Drains pending input and returns true when at least one
    // message was committed. When close_if_empty is true, an empty queue closes
    // acceptance under the same lock, eliminating the final-response race.
    bool drain_active_turn_inputs(bool close_if_empty);
    void append_interrupted_turn_context(const std::string& turn_id);
    std::size_t close_active_turn_and_discard();
    bool maybe_run_auto_compact();
    // 摘要压缩失败后的兜底:改用不调用模型的机械修剪腾出空间。返回 true 表示
    // 空间已经腾出、回合可以继续。见 maybe_run_auto_compact 里的失败分支。
    bool run_mechanical_compact_fallback(int request_tokens,
                                         int context_window,
                                         const std::string& compact_notice_id,
                                         const std::string& summarization_error);
    bool active_estimate_exceeds_auto_threshold(
        const UserInput* pending_input = nullptr) const;

    // 压缩决策该用的窗口。正常等于 context_window_,但当服务端声明的窗口不可
    // 信(实测拒绝过更小的请求)时,收敛到 src/pa 观测到的那条线之下,免得每
    // 一轮都要先撞一次墙才触发恢复。UI 显示的占用百分比不走这里 —— 那里要如实
    // 反映用户配置的窗口,不该被适配层改写。
    int compaction_context_window() const;
    // 当前 provider/model 身份,供上面的观测表按模型分桶。provider 缺席时返回
    // 空串 —— 观测表会把空 model 判为身份不明,既不记录也不查表(见
    // pa::identity_is_known),所以漏接线只会退回原行为,不会串桶。
    void active_model_identity(std::string& provider, std::string& model) const;
    // 服务端整体拒收了这次请求(未产出任何模型输出),把规模记进观测表。
    void note_pa_context_rejection(int request_tokens);
    // 服务端接受了这次请求。只在该模型已经撞过墙时才算 —— 全量 token 估算不
    // 便宜,不该为从未出问题的模型在每个回合上白花一次。
    void note_pa_context_accepted(
        const std::vector<ChatMessage>& messages_with_system);
    std::vector<ChatMessage> build_compaction_initial_context() const;

    // 当前 provider 是否能直接读图。喂给 build_system_prompt 的 # Environment
    // 段,也用于 vision_analyze 的自调用防护。provider 缺席时 fail-open 返回
    // true(与 LlmProvider::supports_vision 默认同口径)。模型切换发生在回合
    // 边界,所以同一回合内多次调用的结果一致,不会打穿 prompt cache 前缀。
    bool active_model_can_read_images() const;
    // 当前 provider 的模型族信息(openspec add-gpt-apply-patch-adaptation):
    // 决定系统提示的工具指引分支与模型侧工具表里给 apply_patch 还是
    // file_edit / file_write。与视觉那一位同口径:只随模型切换变化。
    SystemPromptModelState system_prompt_model_state() const;
    void initialize_compact_window_state();
    void apply_compact_result(const CompactResult& result,
                              const std::string& trigger,
                              const std::string& compact_notice_id);

    // Section 7: 同时调老 on_message callback(若 TUI 挂了)和新事件流
    // (events_)。所有 on_message 触发点都该走这个 helper,确保 daemon
    // 模式下没装 callbacks 也能拿到事件。
    void dispatch_message(const std::string& role,
                          const std::string& content,
                          bool is_tool,
                          nlohmann::json metadata = nlohmann::json::object(),
                          nlohmann::json content_parts = nlohmann::json::array());
    void append_turn_timing_record(const std::string& user_message_uuid,
                                   std::int64_t started_at_ms,
                                   std::int64_t completed_at_ms,
                                   const std::string& status);
    void append_tool_user_prompt(const std::string& content,
                                 const std::string& display_text,
                                 const std::string& source_tool);
    void dispatch_assistant_completed_hook(const ChatMessage& assistant_msg,
                                           const std::shared_ptr<LlmProvider>& provider_snapshot);
    HookCommonPayloadFields build_hook_common_fields(const std::string& event_name) const;
    void apply_hook_side_effects(const HookAggregateOutcome& outcome,
                                 bool include_additional_context = true);
    std::string drain_hook_request_context();
    HookAggregateOutcome dispatch_codex_hook(const std::string& event_name,
                                             const std::string& matcher_value,
                                             const nlohmann::json& payload);

    // ---- Refactored sub-methods of run_agent_with_input ----
    // These decompose the monolithic turn function into focused phases.
    // Return types are defined in the .cpp anonymous namespace.

    // Type alias for the progress emission callback used across sub-methods.
    using ProgressEmitter = std::function<void(
        const std::string& phase, const std::string& label,
        const std::string& detail, const std::string& tool,
        const std::string& tool_call_id, int tool_index, bool force)>;

    // Phase 1: Build user message from input, persist, emit events.
    // Returns turn timing metadata for the orchestrator.
    struct UserTurnInfo {
        ChatMessage user_msg;
        bool visible_timed_turn = false;
        std::string turn_user_uuid;
        std::string active_turn_id;
        std::int64_t turn_started_at_ms = 0;
    };
    UserTurnInfo prepare_user_turn(const UserInput& input, bool hidden_goal_context);
    UserTurnInfo prepare_retry_user_turn(const ChatMessage& message);
    void append_user_turn_message(UserTurnInfo& info, bool hidden_goal_context);
    // 用户消息落盘后把新的会话摘要(无标题时的显示标题)以 session_updated
    // {summary} 推给界面:侧栏与顶部标题栏同源于这一个字段,前端不再各自从
    // 消息正文现推标题(那正是两处标题不一致、且长度不受限的根因)。
    void emit_session_summary_updated();
    void start_user_turn(const UserTurnInfo& info);

    // Phase 2: Build the full message list for the LLM provider.
    struct ApiRequestBundle {
        std::vector<ChatMessage> messages_with_system;
        std::vector<ToolDef> tool_defs;
        ContextUsageBreakdown context_usage_estimate;
        nlohmann::json prompt_diag; // simplified: store as raw json
    };
    ApiRequestBundle build_api_request_messages(bool emergency_profile = false);
    void publish_side_question_context(
        const std::vector<ChatMessage>& messages_with_system);

    // 工具前言 sidecar 任务(add-tool-preamble):detached 线程把结果写进来,
    // AgentLoop 只轮询 / 有界等待,永不被它回调 —— 线程晚于 AgentLoop 结束也
    // 不会踩到已析构的 this。tool_call_ids 由 loop 线程在等待前填。
    struct ToolPreambleSidecarTask {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        std::string title;
        std::vector<std::string> tool_call_ids;
    };
    struct ToolPreambleTitle {
        // 批次标题(reasoning / sidecar 模式):整批工具共用。
        std::string title;
        std::string source;
        std::vector<std::string> tool_call_ids;
        // 逐调用前言(prompt 模式):键 = tool_call_id(空 id 用 "#<index>"),
        // 值 = 该调用自己 `preamble` 参数里的一句话。
        std::map<std::string, std::string> per_call;
    };

    // Phase 3: Stream provider response and accumulate.
    struct ProviderCallResult {
        ChatResponse accumulated;
        bool provider_error_seen = false;
        ProviderErrorInfo provider_error_info;
        std::shared_ptr<LlmProvider> provider_snapshot;
        int provider_attempt = 1;
        // 工具前言:reasoning 模式下流式期间抠到的加粗标题;sidecar 模式下
        // 已启动的旁路摘要任务(可能仍在跑)。
        std::string reasoning_preamble_title;
        std::shared_ptr<ToolPreambleSidecarTask> sidecar_task;
    };
    bool tool_preamble_prompt_mode() const;
    ToolPreambleTitle resolve_tool_preamble_for_step(ProviderCallResult& result);
    void start_tool_preamble_sidecar(ProviderCallResult& result,
                                     const std::string& tool_name,
                                     const std::string& args_preview,
                                     const std::string& assistant_text);
    void flush_late_tool_preamble(bool turn_ending);
    std::string last_user_text_for_preamble() const;
    void emit_tool_preamble_event(const ToolPreambleTitle& preamble, bool late);
    ProviderCallResult call_provider_and_collect(
        const std::shared_ptr<LlmProvider>& provider,
        const ApiRequestBundle& bundle,
        const ProgressEmitter& emit_progress,
        int model_step_index);
    void emit_retry_lifecycle(
        const ProviderErrorInfo& info,
        bool waiting,
        bool compaction);
    void record_terminal_trajectory_events(
        nlohmann::json busy_payload,
        nlohmann::json done_payload);

    // Phase 4: Classify terminal provider errors.
    enum class HandleErrorResult { Continue, Break, Proceed };
    enum class ContextRecoveryStage {
        Normal,
        HistoryRepaired,
        EmergencyProfile,
    };
    HandleErrorResult handle_provider_error(
        ProviderCallResult& result,
        const std::vector<ChatMessage>& messages_with_system,
        std::string& turn_timing_status,
        ContextRecoveryStage& recovery_stage,
        bool& emergency_request_profile);
    // PA 兜底(src/pa/pa_overflow_rescue):服务端以 PA 特征报文拒收整个请求
    // 时,原样重发 → 逐档收缩 → 紧急档 → 等待重发,不因这条报文终止回合。
    // 返回 Continue 表示按新状态重发同一回合;Break 表示等待次数耗尽或用户
    // 中止(调用方按 abort_requested_ 区分)。
    HandleErrorResult run_pa_overflow_rescue(
        const ProviderErrorInfo& error,
        int request_tokens,
        bool& emergency_request_profile);
    // 兜底等待,每 50ms 看一次中止标记。false = 用户中止。
    bool wait_for_pa_rescue_delay(int wait_ms);
    // 兜底等待期间给 TUI / Web 的进度(与 provider 层重试同款展示)。
    void emit_pa_rescue_wait_progress(const ProviderErrorInfo& error,
                                      const pa::RescuePlan& plan,
                                      int attempt,
                                      int max_attempts,
                                      bool waiting);

    // Phase 5: Execute tool calls (parallel read + serial write).
    // Returns true if task_complete terminator fired.
    bool execute_tool_calls(
        const ChatResponse& accumulated,
        const std::shared_ptr<LlmProvider>& provider_snapshot,
        const ProgressEmitter& emit_progress,
        // Mutable state from the orchestrator:
        AgentLoopDoomGuard& doom_guard,
        std::mutex& doom_guard_mu,
        std::string& turn_timing_status);

    // Helper: construct a ToolContext with all callbacks wired up.
    ToolContext build_tool_context(
        const ProgressEmitter& emit_progress,
        AgentLoopDoomGuard& doom_guard,
        std::mutex& doom_guard_mu);

    // Helper: emit agent progress with rate-limiting and coalescing.
    // Uses the progress state passed by reference.
    void emit_progress_tick(
        const ProgressEmitter& emit_progress,
        const std::string& phase, const std::string& label,
        const std::string& detail, const std::string& tool,
        const std::string& tool_call_id, int tool_index, bool force,
        // Mutable progress state:
        std::mutex& progress_mu,
        std::string& active_progress_key,
        std::int64_t& active_progress_started_at_ms,
        std::chrono::steady_clock::time_point& last_progress_emit_at);

    struct WorkerTask {
        enum class Kind { Chat, Shell, Compact, Control };
        Kind kind = Kind::Chat;
        std::string payload;
        UserInput input;
        // 仅 Chat 用:UI 渲染时希望显示的"原文",而 payload(发给 LLM)可能
        // 是被 daemon expander 展开过的字符串(skill 调用提示等)。空 = UI 与
        // LLM 看到同一份(payload)。
        std::string display_text;
        bool hidden_goal_context = false;
        std::function<void()> control;
        std::string retry_user_message_id;
    };

    ProviderAccessor provider_accessor_;
    ToolExecutor& tools_;
    AgentCallbacks callbacks_;
    std::vector<ChatMessage> messages_;
    // Visible events (notably errors and partial output) may not be present
    // in model history or JSONL. They must also invalidate an empty retry.
    std::atomic<bool> live_transcript_tail_blocked_{false};
    mutable std::mutex side_question_context_mu_;
    std::vector<ChatMessage> side_question_context_;
    std::mutex side_question_threads_mu_;
    std::vector<std::thread> side_question_threads_;
    std::atomic<bool> side_question_shutdown_{false};
    std::atomic<bool> abort_requested_{false};
    // Distinguishes a steering interrupt from a manual stop. The former
    // immediately continues with a promised turn and must not pause goals.
    std::atomic<bool> turn_interrupt_requested_{false};
    std::atomic<bool> busy_{false};
    std::mutex active_provider_mu_;
    std::weak_ptr<LlmProvider> active_provider_;
    std::string cwd_;
    mutable sandbox::SandboxRuntime sandbox_runtime_;
    sandbox::ExecRules exec_rules_;
    std::atomic<bool> sandbox_session_disabled_{false};
    // 最近一次 bash 沙盒拒绝(含被拒路径):下一次越权确认据此提供「只放行该目录」
    // 选项(openspec align-codex-sandboxing D4)。bash 成功 / 换 cwd / 沙盒开关时清空。
    std::optional<sandbox::SandboxViolation> last_sandbox_violation_;
    std::string exec_rules_dir_override_;
    std::string global_exec_rules_dir() const;
    // 「批准并记住」:把前缀写进全局规则文件并重载;返回错误信息,空 = 成功。
    std::string remember_exec_rule(const sandbox::ExecPermission& permission);
    void reload_exec_rules();
    // 写一条审计(补 ts / session_id / cwd 后交给 audit_sink_)。异常一律吞掉:
    // 审计失败不能影响工具执行。
    void record_audit(const std::string& category, const std::string& tool,
                      const std::string& target, const std::string& decision,
                      const std::string& source, const std::string& reason,
                      const std::string& sandbox = {},
                      nlohmann::json detail = nlohmann::json::object());
    security::AuditSink audit_sink_;
    std::string sandbox_prompt_description() const;
    mutable std::mutex sandbox_prompt_mutex_;
    mutable std::optional<std::pair<PermissionMode, std::string>> sandbox_prompt_snapshot_;
    PermissionManager& permissions_;
    PathValidator path_validator_;
    // 「编辑项目」的主文件夹与附加文件夹快照(refresh_workspace_folders 每回合重读)。
    // 并行只读工具会在工作线程上查它,用锁保护。
    mutable std::mutex workspace_folders_mu_;
    std::string workspace_main_folder_;
    std::vector<std::string> workspace_extra_folders_;
    // 路径落在某个可写附加文件夹内(相对路径按 cwd_ 解析)。
    bool path_in_workspace_folders(const std::string& path) const;
    // 系统提示 # Environment 的附加工作目录两行(可写 / 本会话只读)。
    SystemPromptWorkspaceFolders system_prompt_workspace_folders() const;
    std::atomic<int> context_window_{128000};
    std::string no_model_config_prompt_;
    // agent_loop termination policy. Fresh defaults come from AgentLoopConfig
    // until set_agent_loop_config is called from main.cpp.
    AgentLoopConfig loop_cfg_;
    // 工具前言(add-tool-preamble)状态。配置 / 摘要器受 tool_preamble_mu_ 保护
    // (设置页可在回合中途改);其余两项只在 worker 线程上读写。
    mutable std::mutex tool_preamble_mu_;
    ToolPreambleConfig tool_preamble_cfg_;
    std::atomic<bool> jb_mode_{false};
    ToolPreambleSidecarSummarizer tool_preamble_summarizer_;
    // 本模型步解析出的标题:run_agent_with_input 在 Phase 5 之前填,
    // execute_tool_calls 开头消费(挂 metadata + 发事件)后清空。
    ToolPreambleTitle current_step_preamble_;
    // 落盘前没等到结果的旁路任务:工具执行完再看一眼,到了就补发 late 事件。
    std::shared_ptr<ToolPreambleSidecarTask> late_sidecar_task_;
    LoopExecutionPolicy loop_execution_policy_;
    // spawn_subagent 透传的父会话写边界根;见 write_root()。
    std::string inherited_write_root_;
    static constexpr int kTurnOutcomeNone = 0;
    static constexpr int kTurnOutcomeCompleted = 1;
    static constexpr int kTurnOutcomeError = 2;
    static constexpr int kTurnOutcomeAborted = 3;
    // 写在 busy_ 翻 false 之前,轮询 is_busy() 的 wait_for_subagent 一看到
    // 空闲就能读到确定的结果。
    std::atomic<int> last_turn_outcome_{kTurnOutcomeNone};
    mutable std::mutex last_turn_error_mu_;
    std::string last_turn_error_;
    void record_turn_outcome(const std::string& turn_timing_status);
    // Latest server-reported total active-context usage. For providers that do
    // not return total_tokens, prompt_tokens is used as the fallback.
    std::atomic<int> last_api_total_tokens_{0};
    // Aggregate usage for the regular turn currently owned by the worker.
    // Kept as worker state (rather than a stack local) so the outer worker
    // recovery boundary can still publish an accurate terminal summary after
    // an exception unwinds run_agent_with_input().
    TokenUsage active_turn_usage_;
    bool active_turn_usage_initialized_ = false;
    // PA 兜底的 episode 进度(见 run_pa_overflow_rescue)。服务端收下请求即
    // 清零;回合开始也清零。只在回合线程上读写。
    pa::RescueState pa_rescue_state_;
    // 兜底刚做完一步之后的那次重发跳过自动压缩:压缩本身又是一次可能被拒的
    // 模型请求,先把已经缩好的请求发出去;下一次采样再照常压缩。
    bool skip_auto_compact_once_ = false;
    std::atomic<int> compact_generation_{0};
    std::atomic<int> task_suggestion_compact_threshold_{3};
    bool compact_window_initialized_ = false;
    std::uint64_t compact_window_number_ = 0;
    std::string compact_first_window_id_;
    std::string compact_current_window_id_;
    SessionManager* session_manager_ = nullptr;
    HookManager* hook_manager_ = nullptr;
    std::vector<std::string> hook_request_context_;
    bool stop_hook_active_ = false;
    const SkillRegistry* skill_registry_ = nullptr;
    SkillUsageStore* skill_usage_store_ = nullptr;
    int skill_idle_days_ = 30;
    const MemoryRegistry* memory_registry_ = nullptr;
    const MemoryConfig* memory_cfg_ = nullptr;
    const ProjectInstructionsConfig* project_instructions_cfg_ = nullptr;
    const CustomInstructionsConfig* custom_instructions_cfg_ = nullptr;
    const ExpertDefinition* expert_ = nullptr;
    std::string expert_member_id_;
    ToolCapabilityPolicy tool_capability_policy_;
    const GitContextConfig* git_context_cfg_ = nullptr;
    // gitStatus 快照缓存(openspec add-git-context):nullopt = 尚未采集,
    // 空串 = 已采集但非仓库/失败/disabled(不注入)。只在 worker 线程读写
    // (build_api_request_messages 惰性采集,set_cwd 经工具回调在同线程重置),
    // 与 cwd_ 本身的线程假设一致。
    std::optional<std::string> git_snapshot_cache_;
    // 跨线程失效信号(invalidate_git_snapshot):worker 在模型请求前 exchange
    // 消费,避免 HTTP 线程直接 reset optional 造成数据竞争。
    std::atomic<bool> git_snapshot_stale_{false};
    std::string session_context_cache_key_;
    std::string session_context_cache_content_;
    std::string skill_context_cache_key_;
    std::string skill_context_cache_content_;
    // Worker-thread-only flag derived from the current root UserInput. It
    // remains active across all provider iterations in that turn.
    bool active_turn_swarm_mode_ = false;
    // Worker-only control populated by terminal tool results. Actions run only
    // after canonical tool results, turn timing, BusyChanged and Done have all
    // been emitted/persisted.
    bool terminate_session_after_turn_ = false;
    std::vector<std::function<void()>> post_turn_actions_;
    std::string goal_accounting_thread_id_;
    std::string goal_accounting_goal_id_;
    std::string budget_notice_goal_id_;
    std::chrono::steady_clock::time_point goal_time_checkpoint_{};
    // Goal steering pending 标记。atomic:budget 标记可能从并行读工具批次的
    // 线程置位,objective 标记从 TUI/daemon 命令线程置位;消费固定在 worker
    // 线程的模型请求前。回合开始时清零 = Codex inject_if_running 失败即丢弃。
    std::atomic<bool> pending_goal_budget_limit_steering_{false};
    std::atomic<bool> pending_goal_objective_steering_{false};
    std::map<std::string, std::chrono::steady_clock::time_point> recent_safe_edit_failures_;

    static constexpr std::size_t kMaxPendingTurnSteers = 128;
    mutable std::mutex active_turn_mu_;
    std::string active_turn_id_;
    bool active_turn_accepting_ = false;
    std::deque<UserInput> pending_turn_inputs_;

    // Worker thread and task queue
    std::thread worker_thread_;
    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    // Immediate steering follow-ups run before ordinary queued work. Separate
    // FIFOs preserve both urgent and ordinary relative ordering.
    std::queue<WorkerTask> priority_task_queue_;
    std::queue<WorkerTask> task_queue_;
    std::set<std::string> task_suggestion_input_ids_;
    bool shutdown_requested_ = false;
    bool worker_task_active_ = false;
    WorkerTask::Kind worker_task_kind_ = WorkerTask::Kind::Control;
    std::uint64_t next_control_sequence_ = 0;

    // Section 7: 事件分发器。EventDispatcher 自己内部加锁,所以这里不需要
    // 额外的同步;emit 由 worker_main 线程调用,subscribe/unsubscribe 由
    // HTTP handler 线程并发调用。
    EventDispatcher events_;

    // Section 7.6: PermissionPrompter。null 时走 callbacks_.on_tool_confirm
    // 老路径(TUI);非 null 时(daemon 模式)走 prompter_->prompt。
    std::unique_ptr<PermissionPrompter> prompter_;

    // AskUserQuestionPrompter: daemon 模式下走 WS。raw 指针,生命周期由
    // SessionEntry 持有。null 时 ToolContext::ask_user_questions 不注入,
    // 此时 AskUserQuestion 工具(daemon 工厂版)会返回 rejected。
    AskUserQuestionPrompter* ask_prompter_ = nullptr;
    AskQuestionChannel ask_channel_;
};

} // namespace acecode
