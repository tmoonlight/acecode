#include "turn_runner.hpp"
#include "turn_context.hpp"
#include "turn_lifecycle.hpp"
#include "turn_model_step_sink.hpp"
#include "assistant_output.hpp"
#include "agent/approval/session_exec_security.hpp"
#include "agent/boundary/workspace_boundary.hpp"
#include "agent/compaction/compaction_controller.hpp"
#include "agent/detail/agent_payloads.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "agent/hook_bridge/agent_hook_bridge.hpp"
#include "agent/model_step/model_step_recorder.hpp"
#include "agent/model_step/provider_stream_collector.hpp"
#include "agent/progress/activity_narrator.hpp"
#include "agent/progress/agent_progress_emitter.hpp"
#include "agent/request/request_context_source.hpp"
#include "agent/side_question/side_question_service.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/turn/turn_outcome.hpp"
#include "agent/compaction/compact.hpp"
#include "agent/transcript/transcript_queries.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "session/event_dispatcher.hpp"
#include "session/inter_agent_message.hpp"
#include "session/session_manager.hpp"
#include "session/system_notice.hpp"
#include "config/config.hpp"
#include "utils/abort_signal.hpp"
#include "utils/logger.hpp"
#include <utility>

namespace acecode::agent {
using detail::kDefaultNoModelConfiguredPrompt;

namespace {
// 输出损坏只重发一次:网关偶发把模板残片吐进输出流,重采通常就好;连续两次
// 损坏多半是服务端持续异常,停止并报错,绝不放行损坏回复中的工具调用。
constexpr int kMaxCorruptedOutputRetries = 1;
} // namespace

void TurnRunner::discard_corrupted_output(const ChatResponse& response,
    const std::string& marker, int attempt, int attempts, bool retrying) {
    LOG_WARN("Corrupted model output: leaked template markup " + marker +
             " in reply text; discarding the step without running its " +
             std::to_string(response.tool_calls.size()) +
             " tool call(s); retrying=" + (retrying ? "true" : "false") +
             " attempt=" + std::to_string(attempt) + "/" +
             std::to_string(attempts) + " excerpt=" +
             log_truncate(response.content, 300));
    // 与 provider 的 Retry 事件同一套清理:TUI 丢掉流式草稿行,Web 用已落盘
    // 的消息整体替换转录,损坏的正文不留在界面上。
    const auto callbacks = callbacks_.snapshot();
    if (callbacks.on_stream_retry_reset) callbacks.on_stream_retry_reset();
    const std::vector<ChatMessage> visible =
        session_ ? session_->load_active_messages() : history_.view();
    events_.emit(SessionEventKind::TranscriptReplace,
        detail::build_transcript_replace_payload(visible, CompactResult{}));
    if (!retrying) {
        transcript_.dispatch_message("error",
            "[输出异常] 模型重试后仍返回损坏的工具模板，已丢弃回复并停止，"
            "未执行其中的工具调用。请重试或切换模型。",
            false, nlohmann::json::object(), nlohmann::json::array());
        return;
    }
    transcript_.emit_transcript_system_message(session_,
        std::string(u8"[输出异常] 模型回复里混入了工具参数模板标记(") + marker +
            u8"),正文与工具调用都不可信,已丢弃并重新请求 " +
            std::to_string(attempt) + "/" + std::to_string(attempts) + u8"…",
        make_system_notice_metadata("response_corrupted_retry",
            {{"attempt", attempt}, {"attempts", attempts}, {"marker", marker}}));
}

void TurnRunner::run(TurnContext& turn, const UserInput& input, bool hidden_goal_context,
                     const ChatMessage* retry_message, LifetimeRef<TrajectoryRecorder> terminal) {
    // Capture the owner before callbacks can switch/delete the active session.
    // RAII also releases on exceptions and early hook returns.
    turn.desktop_lease = source_.runtime.computer_use_lease(
        session_ ? session_->current_session_id() : std::string{},
        options_.computer_use_release);
    auto& desktop_turn_lease = *turn.desktop_lease;
    // P0-11:进度节流的取时函数在回合开始时按值捕获,回合内不再读取可配置成员。
    turn.progress_clock = options_.clock;
    security_.reset_prompt_snapshot();
    // 「编辑项目」保存的附加文件夹:每回合开头重读,放在沙盒描述快照之前,
    // 本回合的系统提示与可写根一致且回合内不变(prompt cache 前缀稳定)。
    boundary_.refresh_workspace_folders(session_);
    security_.runtime().set_workspace_writable_roots(boundary_.writable_workspace_folders(session_));
    abort_.clear();
    interrupt_ = false;
    busy_ = true;
    outcome_.begin();
    turn.tools.terminate_session_after_turn = false;
    turn.tools.post_turn_actions.clear();
    goal_.restore(session_);
    // 上一回合没来得及消费的 steering 标记直接丢弃(等价 Codex
    // inject_if_running 在无活动回合时静默跳过)。
    goal_.begin_turn();

    // 跨 agent 信封不是用户输入:不触发 UserPromptSubmit 钩子(与隐藏 goal 上下文同理)。
    const bool inter_agent_input =
        mesh::inter_agent_envelope_from_metadata(input.metadata).has_value();
    if (!hidden_goal_context && !inter_agent_input && hook_manager_) {
        auto fields = hooks_.common_fields(kCodexHookEventUserPromptSubmit, session_);
        auto payload = build_user_prompt_submit_hook_payload(fields, input.text);
        auto outcome = hooks_.dispatch(hook_manager_,
            kCodexHookEventUserPromptSubmit, std::string{}, payload);
        hooks_.apply(outcome);
        if (outcome.blocked || outcome.denied) {
            const std::string reason = outcome.reason.empty()
                ? "User prompt blocked by hook."
                : outcome.reason;
            finalizer_.hook_blocked(turn, reason, terminal);
            return;
        }
    }

    // Codex pre-turn compaction estimates the pending input but summarizes only
    // already-recorded history. Persisting first would put the new request into
    // the summary and append the checkpoint after the input, breaking replay.
    auto& preturn_compaction_failed = turn.preturn_compaction_failed;
    if (!retry_message && compaction_.exceeds_auto_threshold(compaction_inputs(turn, terminal), &input)) {
        preturn_compaction_failed = !compaction_.run_auto(compaction_inputs(turn, terminal));
    }

    // Phase 1: Build and persist user message after the pre-turn compact attempt.
    agent::TurnLifecycle lifecycle(history_, transcript_, gate_,
        events_, callbacks_, session_, source_.skills.get(), source_.skill_usage);
    turn.info = retry_message
        ? lifecycle.prepare_retry_user_turn(*retry_message)
        : lifecycle.prepare_user_turn(input, hidden_goal_context);
    if (session_) desktop_turn_lease.set_owner(session_->current_session_id());
    auto& turn_timing_status = turn.timing_status;
    if (preturn_compaction_failed) {
        turn_timing_status = "error";
        goal_.stop_after_error(session_, ProviderErrorInfo{});
    }

    // Loop state
    auto& total_iterations = turn.total_iterations;
    auto& terminator_fired = turn.terminator_fired;
    turn.recovery = agent::RequestRecoveryState{};
    auto& context_recovery_stage = turn.recovery.stage;
    auto& emergency_request_profile = turn.recovery.emergency_profile;
    activity_.reset_turn();

    const int max_iter = config_.max_iterations;
    const bool has_max_iterations = max_iter > 0;
    agent::ResponseRecovery response_recovery(history_, transcript_, goal_, session_);
    agent::AssistantOutput assistant_output(
        history_, transcript_, side_questions_, hooks_, steps_);
    auto& doom_guard = turn.doom_guard;
    auto& observed_compact_generation = turn.observed_compact_generation;
    observed_compact_generation = compaction_.generation();
    auto reset_doom_guard_after_compact = [&]() {
        const int current_generation = compaction_.generation();
        if (current_generation == observed_compact_generation) return;
        doom_guard.reset();
        observed_compact_generation = current_generation;
        LOG_INFO("Doom guard reset after compact generation " +
                 std::to_string(current_generation));
    };

    // The turn owns this shared emitter; tool callbacks retain its state by
    // value, so no callback captures the emitter's stack locals.
    turn.progress = std::make_shared<agent::AgentProgressEmitter>(
        activity_, events_, turn.progress_clock);
    auto emit_agent_progress = [progress = turn.progress](const std::string& phase,
                                         const std::string& label,
                                         const std::string& detail = std::string{},
                                         const std::string& tool = std::string{},
                                         const std::string& tool_call_id = std::string{},
                                         int tool_index = -1,
                                         bool force = false) {
        progress->emit(phase, label, detail, tool, tool_call_id, tool_index, force);
    };

    auto maybe_continue_from_stop_hook = [&](const std::string& last_assistant_message) {
        return hooks_.continue_from_stop(hook_manager_, session_, last_assistant_message);
    };

    auto& model_step_index = turn.model_step_index;
    // Main agent loop
    while (!preturn_compaction_failed && !abort_.raw() && !terminator_fired &&
           (!has_max_iterations || total_iterations < max_iter)) {
        ++total_iterations;
        doom_guard.begin_model_turn();
        reset_doom_guard_after_compact();
        LOG_INFO("--- Agent loop turn " + std::to_string(total_iterations) +
                 ", messages: " + std::to_string(history_.view().size()));

        if (abort_.raw()) {
            LOG_WARN("Abort requested, breaking loop");
            break;
        }

        // The top of every sampling iteration covers both pre-turn and
        // post-tool follow-up compaction. A failed compact aborts this sampling
        // path without silently deleting unsummarized history.
        // PA 兜底刚做完一步的那次重发不压缩(见 turn.recovery.skip_auto_compact_once);
        // 这个标记只管紧接着的一次采样,重发成功后的下一次采样照常压缩。
        const bool skip_auto_compact_after_rescue = turn.recovery.skip_auto_compact_once;
        turn.recovery.skip_auto_compact_once = false;
        if (total_iterations > 1 && !skip_auto_compact_after_rescue &&
            context_recovery_stage == ContextRecoveryStage::Normal &&
            compaction_.exceeds_auto_threshold(compaction_inputs(turn, terminal), nullptr)) {
            if (!compaction_.run_auto(compaction_inputs(turn, terminal))) {
                turn_timing_status = "error";
                goal_.stop_after_error(session_, ProviderErrorInfo{});
                break;
            }
            reset_doom_guard_after_compact();
        }

        // Goal steering:budget_limit / objective_updated 提示在下一次模型
        // 请求前注入(hidden_goal_context user 消息,进 API 与持久化,UI 不显示)。
        drain_inputs(false);
        goal_.inject_steering(session_, {
            tools_.is_allowed("update_goal", &source_.tool_policy),
            tools_.is_allowed("AskUserQuestion", &source_.tool_policy)});

        // One provider lease per iteration: prompt facts and chat use the
        // same snapshot. The null-provider decision remains before StepStart.
        std::shared_ptr<LlmProvider> provider_snapshot;
        if (options_.provider) provider_snapshot = options_.provider();
        // Phase 2: Build API request messages
        auto bundle = requests_.build(provider_snapshot, emergency_request_profile);
        side_questions_.publish(bundle.messages_with_system);
        turn.model_tool_names.clear();
        turn.model_tool_names.reserve(bundle.tool_defs.size());
        for (const auto& def : bundle.tool_defs) {
            turn.model_tool_names.push_back(def.name);
        }

        // Check the same snapshot after publishing the detached context.
        if (!provider_snapshot) {
            LOG_ERROR("provider_accessor returned null; aborting turn");
            turn_timing_status = "error";
            transcript_.dispatch_message(
                "error",
                options_.no_model_prompt.empty()
                    ? kDefaultNoModelConfiguredPrompt
                    : options_.no_model_prompt,
                false, nlohmann::json::object(), nlohmann::json::array());
            goal_.stop_after_error(session_, ProviderErrorInfo{});
            break;
        }

        // Phase 3: Call provider and collect response.这两个显式 lifecycle 事件
        // 是完成态 JSONL 的可靠边界;progress/usage 都不能替代它们。
        const int current_model_step = ++model_step_index;
        steps_.start(current_model_step);
        steps_.request(
            session_, current_model_step, provider_snapshot, bundle, context_window_.load());
        TurnModelStepSink sink(turn, usage_, steps_, session_);
        auto provider_result = stream_.collect(
            provider_snapshot, bundle, emit_agent_progress, current_model_step, sink, session_);
        TokenUsage step_usage = provider_result.accumulated.usage;

        if (abort_.raw()) {
            assistant_output.interrupted(provider_result.accumulated, session_);
            steps_.response(session_,
                current_model_step, provider_result, step_usage, "aborted");
            steps_.finish(current_model_step, "aborted", step_usage);
            break;
        }

        // Phase 4: Handle provider errors (context rescue, fatal errors)
        const auto error = recovery_.resolve(provider_result, bundle.messages_with_system,
            turn.recovery, context_window_.load(), session_);
        if (error.timing_status) turn_timing_status = *error.timing_status;
        const auto error_result = error.decision;
        reset_doom_guard_after_compact();
        if (error_result == HandleErrorResult::Continue) {
            steps_.response(session_,
                current_model_step, provider_result, step_usage, "retry");
            steps_.finish(current_model_step, "retry", step_usage);
            --total_iterations;
            continue;
        }
        if (error_result == HandleErrorResult::Break) {
            steps_.response(session_,
                current_model_step, provider_result, step_usage, "error");
            steps_.finish(current_model_step, "error", step_usage);
            break;
        }

        // Phase 4b: 输出损坏 —— 正文里混进了工具参数模板标记(<arg_value> 等)。
        // 这一步的正文和工具调用都不可信:整条丢弃(不入历史、不执行工具)、
        // 原样重发一次;重发后仍损坏就停止。反馈 huangyuan816:第一条
        // 回复是一串数字加 </arg_value>,同一回复里的 bash 命令也夹着乱码,照样
        // 被执行,之后整个回合跑题。检查放在 has_tool_calls 分支之前,带原生
        // 工具调用的回复同样拦下。
        if (const auto marker = find_leaked_tool_argument_markup(
                provider_result.accumulated.content)) {
            const bool retrying = turn.response_recovery.corrupted_output_retries <
                                  kMaxCorruptedOutputRetries;
            if (retrying) {
                ++turn.response_recovery.corrupted_output_retries;
            }
            discard_corrupted_output(provider_result.accumulated, *marker,
                turn.response_recovery.corrupted_output_retries,
                kMaxCorruptedOutputRetries, retrying);
            const std::string status = retrying ? "retry" : "error";
            steps_.response(session_, current_model_step, provider_result, step_usage, status);
            steps_.finish(current_model_step, status, step_usage);
            if (!retrying) {
                turn_timing_status = "error";
                goal_.stop_after_error(session_, ProviderErrorInfo{});
                break;
            }
            if (total_iterations > 0) --total_iterations;
            continue;
        }

        // Provider IDs can repeat between responses. Reserve distinct execution
        // IDs before traces, hooks, lifecycle events, persistence and callbacks.
        history_.prepare_tool_calls(provider_result.accumulated.tool_calls);

        // Usage estimation when provider didn't report usage。必须覆盖所有轮:
        // 旧条件把「纯工具调用轮(无正文)」排除,导致不上报 usage 的
        // provider 下 goal 预算在工具轮从不入账,budget_limited 永不触发。
        if (!provider_result.accumulated.usage.has_data) {
            step_usage = usage_.estimate(
                turn.usage, provider_result.accumulated, bundle, session_);
        }
        steps_.response(session_,
            current_model_step, provider_result, step_usage, "completed");

        // Text-only response (no tool calls) → end the loop
        if (!provider_result.accumulated.has_tool_calls()) {
            const auto decision = response_recovery.resolve(
                provider_result.accumulated, turn.response_recovery, turn.model_tool_names);
            if (decision.action == HandleErrorResult::Continue) {
                if (total_iterations > 0) --total_iterations;
                steps_.finish(current_model_step, decision.finish_status, step_usage);
                continue;
            }
            if (decision.action == HandleErrorResult::Break) {
                turn_timing_status = "error";
                steps_.finish(current_model_step, decision.finish_status, step_usage);
                break;
            }
            assistant_output.completed(
                provider_result.accumulated, bundle, provider_snapshot,
                current_model_step, step_usage, session_, hook_manager_);
            if (maybe_continue_from_stop_hook(provider_result.accumulated.content)) {
                continue;
            }
            if (drain_inputs(true)) {
                continue;
            }
            break;
        }

        // 本轮产出了有效输出(工具调用),连续空回复 / 文本调用纠正计数清零。
        turn.response_recovery = agent::ResponseRecoveryState{};

        // 工具前言(add-tool-preamble):在 assistant(tool_calls) 消息落盘之前把
        // 本步沿用的阶段前言定下来,execute_tool_calls 开头把它挂进 metadata
        // 并随 tool_start 下发。
        turn.preamble = activity_.resolve_step(provider_result.accumulated);

        // Phase 5: Execute tool calls
        auto tool_batch_outcome = execute_tools(
            turn, provider_result.accumulated, provider_snapshot, emit_agent_progress);
        terminator_fired = tool_batch_outcome.terminator_fired;
        turn.tools.terminate_session_after_turn |= tool_batch_outcome.terminate_session_after_turn;
        for (auto& action : tool_batch_outcome.post_turn_actions) {
            turn.tools.post_turn_actions.push_back(std::move(action));
        }
        if (provider_result.accumulated.text_tool_calls.outcome ==
                TextToolCallDiagnostic::Outcome::IgnoredWithNative &&
            !terminator_fired && !abort_.raw()) {
            assistant_output.ignored_text_call(
                provider_result.accumulated.text_tool_calls, session_);
        }
        steps_.finish(
            current_model_step, provider_result.accumulated.finish_reason,
            step_usage);
        if (!turn.tools.terminate_session_after_turn && terminator_fired &&
            maybe_continue_from_stop_hook(provider_result.accumulated.content)) {
            terminator_fired = false;
            continue;
        }
        if (!turn.tools.terminate_session_after_turn && terminator_fired &&
            drain_inputs(true)) {
            terminator_fired = false;
            continue;
        }
    }

    finalizer_.normal(turn, max_iter, terminal);

}

} // namespace acecode::agent
