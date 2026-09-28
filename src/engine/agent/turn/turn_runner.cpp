#include "agent/agent_loop.hpp"
#include "agent/recovery/context_overflow_recovery.hpp"
#include "agent/compaction/compaction_controller.hpp"
#include "agent/model_step/model_step_recorder.hpp"
#include "agent/model_step/turn_usage_accountant.hpp"
#include "agent/progress/activity_narrator.hpp"
#include "agent/progress/agent_progress_emitter.hpp"
#include "agent/hook_bridge/agent_hook_bridge.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "agent/approval/session_exec_security.hpp"
#include "agent/turn/turn_outcome.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "computer_use/session_lease.hpp"
#include "agent/detail/agent_payloads.hpp"
#include "agent/event_payload/message_payload.hpp"
#include "agent/guards/doom_guard.hpp"
#include "agent/recovery/provider_error_report.hpp"
#include "agent/tool_exec/tool_batch_types.hpp"
#include "agent/transcript/transcript_queries.hpp"
#include "hooks/hook_manager.hpp"
#include "hooks/hook_runtime.hpp"
#include "llm/tool_protocol_names.hpp"
#include "permissions/interaction_mode.hpp"
#include "permissions/shell_write_guard.hpp"
#include "prompt/context_usage_breakdown.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/output_attachments.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_rewind.hpp"
#include "session/session_serializer.hpp"
#include "session/session_storage.hpp"
#include "session/system_notice.hpp"
#include "session/task_suggestion_store.hpp"
#include "session/thread_goal_store.hpp"
#include "session/thread_repair.hpp"
#include "session/token_tracker.hpp"
#include "session/turn_net_diff.hpp"
#include "session/turn_timing.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"
#include "utils/text.hpp"
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

using agent::detail::build_agent_progress_payload;
using agent::detail::model_step_usage_to_json;
using agent::detail::accumulate_turn_usage;
using agent::detail::text_tool_call_diagnostic_to_json;
using agent::detail::text_tool_call_rejected_persisted_content;
using agent::detail::trailing_transcript_message;
using agent::detail::provider_error_to_json;
using agent::detail::kDefaultNoModelConfiguredPrompt;
using utils::now_epoch_ms;

void AgentLoop::run_agent_with_input(const UserInput& input,
                                      bool hidden_goal_context,
                                      const ChatMessage* retry_message) {
    // Capture the owner before callbacks can switch/delete the active session.
    // RAII also releases on exceptions and early hook returns.
    computer_use::SessionLease desktop_turn_lease(
        session_manager_ ? session_manager_->current_session_id() : std::string{},
        computer_use_release_);
    // P0-11:进度节流的取时函数在回合开始时按值捕获,回合内不再读取可配置成员。
    turn_progress_clock_ = progress_clock_;
    exec_security_->reset_prompt_snapshot();
    // 「编辑项目」保存的附加文件夹:每回合开头重读,放在沙盒描述快照之前,
    // 本回合的系统提示与可写根一致且回合内不变(prompt cache 前缀稳定)。
    refresh_workspace_folders();
    abort_signal_.clear();
    turn_interrupt_requested_ = false;
    busy_ = true;
    turn_outcome_->begin();
    terminate_session_after_turn_ = false;
    post_turn_actions_.clear();
    restore_goal_runtime();
    // 上一回合没来得及消费的 steering 标记直接丢弃(等价 Codex
    // inject_if_running 在无活动回合时静默跳过)。
    goal_->begin_turn();
    active_turn_swarm_mode_ = false;

    if (!hidden_goal_context && hook_manager_) {
        auto fields = build_hook_common_fields(kCodexHookEventUserPromptSubmit);
        auto payload = build_user_prompt_submit_hook_payload(fields, input.text);
        auto outcome = dispatch_codex_hook(
            kCodexHookEventUserPromptSubmit, std::string{}, payload);
        apply_hook_side_effects(outcome);
        if (outcome.blocked || outcome.denied) {
            const std::string reason = outcome.reason.empty()
                ? "User prompt blocked by hook."
                : outcome.reason;
            dispatch_message("error", "[Hook blocked prompt] " + reason, false);
            account_goal_usage(0, false);
            if (callbacks_.on_turn_finished) {
                callbacks_.on_turn_finished("error");
            }
            const std::string turn_id = generate_uuid();
            const auto usage = model_step_usage_to_json(turn_usage_->aggregate);
            const nlohmann::json idle = {
                {"busy", false},
                {"outcome", "error"},
                {"turn_id", turn_id},
                {"usage", usage},
            };
            const nlohmann::json done = {
                {"outcome", "error"},
                {"turn_id", turn_id},
                {"usage", usage},
            };
            record_terminal_trajectory_events(idle, done);
            if (callbacks_.on_busy_changed) callbacks_.on_busy_changed(false);
            record_turn_outcome("error");
            busy_ = false;
            events_.emit(SessionEventKind::BusyChanged, idle);
            events_.emit(SessionEventKind::Done, done);
            maybe_continue_goal();
            return;
        }
    }

    active_turn_swarm_mode_ =
        input.metadata.is_object() &&
        input.metadata.contains("swarm_mode") &&
        input.metadata["swarm_mode"].is_boolean() &&
        input.metadata["swarm_mode"].get<bool>();
    struct ActiveTurnSwarmModeReset {
        bool& active;
        ~ActiveTurnSwarmModeReset() { active = false; }
    } swarm_mode_reset{active_turn_swarm_mode_};

    // Codex pre-turn compaction estimates the pending input but summarizes only
    // already-recorded history. Persisting first would put the new request into
    // the summary and append the checkpoint after the input, breaking replay.
    bool preturn_compaction_failed = false;
    if (!retry_message && active_estimate_exceeds_auto_threshold(&input)) {
        preturn_compaction_failed = !maybe_run_auto_compact();
    }

    // Phase 1: Build and persist user message after the pre-turn compact attempt.
    auto turn_info = retry_message
        ? prepare_retry_user_turn(*retry_message)
        : prepare_user_turn(input, hidden_goal_context);
    if (session_manager_) desktop_turn_lease.set_owner(session_manager_->current_session_id());
    std::string turn_timing_status = "completed";
    if (preturn_compaction_failed) {
        turn_timing_status = "error";
        stop_active_goal_after_turn_error(ProviderErrorInfo{});
    }

    // Loop state
    int total_iterations = 0;
    bool terminator_fired = false;
    *recovery_state_ = agent::RequestRecoveryState{};
    auto& context_recovery_stage = recovery_state_->stage;
    auto& emergency_request_profile = recovery_state_->emergency_profile;
    activity_->reset_turn();

    const int max_iter = loop_cfg_.max_iterations;
    const bool has_max_iterations = max_iter > 0;
    // 空回复兜底重试(fix-glm-empty-response-turn-end):HTTP 200 + [DONE] 正常
    // 收尾、但 content/tool_calls 全空的「成功空响应」。实测形态:火山引擎 GLM
    // 深度思考把输出 token 预算全部耗在 reasoning 上(finish_reason=length,
    // 但部分网关不上报该字段,因此不能依赖它触发),正文与工具调用没机会输出,
    // 旧行为被 text-only 分支当作正常回复静默终止回合。连续空回复才累计,
    // 一旦某轮产出有效输出(文本或工具调用)即清零。
    constexpr int kMaxEmptyResponseRetries = 2;
    int empty_response_retries = 0;
    // 文本形式工具调用纠正(fix-feedback-0924 第 3 条):模型把调用写进正文、
    // provider 认出了意图却无法执行(非法名 / 参数 / 截断 / 块前有正文 / 可疑
    // 标记)时,注入隐藏纠正提示重试。按连续次数计,某一步产出工具调用即清零;
    // XML / JSON 形态上限 2 次(与空回复一致,每次都是带全量工具表的整段请求),
    // DSML 只 1 次(那是网关把 DeepSeek 原生协议漏进了正文,纠正文本作用有限)。
    constexpr int kMaxTextToolCallCorrections = 2;
    constexpr int kMaxDsmlToolCallCorrections = 1;
    int text_tool_call_corrections = 0;
    agent::SynchronizedDoomGuard doom_guard;
    int observed_compact_generation = compaction_->generation();
    auto reset_doom_guard_after_compact = [&]() {
        const int current_generation = compaction_->generation();
        if (current_generation == observed_compact_generation) return;
        doom_guard.reset();
        observed_compact_generation = current_generation;
        LOG_INFO("Doom guard reset after compact generation " +
                 std::to_string(current_generation));
    };

    // The turn owns this shared emitter; tool callbacks retain its state by
    // value, so no callback captures the emitter's stack locals.
    auto progress = std::make_shared<agent::AgentProgressEmitter>(
        *activity_, events_, turn_progress_clock_);
    auto emit_agent_progress = [progress](const std::string& phase,
                                         const std::string& label,
                                         const std::string& detail = std::string{},
                                         const std::string& tool = std::string{},
                                         const std::string& tool_call_id = std::string{},
                                         int tool_index = -1,
                                         bool force = false) {
        progress->emit(phase, label, detail, tool, tool_call_id, tool_index, force);
    };

    auto maybe_continue_from_stop_hook = [&](const std::string& last_assistant_message) {
        return hooks_->continue_from_stop(hook_manager_, session_manager_, last_assistant_message);
    };

    int model_step_index = 0;
    // Main agent loop
    while (!preturn_compaction_failed && !abort_signal_.raw() && !terminator_fired &&
           (!has_max_iterations || total_iterations < max_iter)) {
        ++total_iterations;
        doom_guard.begin_model_turn();
        reset_doom_guard_after_compact();
        LOG_INFO("--- Agent loop turn " + std::to_string(total_iterations) +
                 ", messages: " + std::to_string(history_->view().size()));

        if (abort_signal_.raw()) {
            LOG_WARN("Abort requested, breaking loop");
            break;
        }

        // The top of every sampling iteration covers both pre-turn and
        // post-tool follow-up compaction. A failed compact aborts this sampling
        // path without silently deleting unsummarized history.
        // PA 兜底刚做完一步的那次重发不压缩(见 recovery_state_->skip_auto_compact_once);
        // 这个标记只管紧接着的一次采样,重发成功后的下一次采样照常压缩。
        const bool skip_auto_compact_after_rescue = recovery_state_->skip_auto_compact_once;
        recovery_state_->skip_auto_compact_once = false;
        if (total_iterations > 1 && !skip_auto_compact_after_rescue &&
            context_recovery_stage == ContextRecoveryStage::Normal &&
            active_estimate_exceeds_auto_threshold()) {
            if (!maybe_run_auto_compact()) {
                turn_timing_status = "error";
                stop_active_goal_after_turn_error(ProviderErrorInfo{});
                break;
            }
            reset_doom_guard_after_compact();
        }

        // Goal steering:budget_limit / objective_updated 提示在下一次模型
        // 请求前注入(hidden_goal_context user 消息,进 API 与持久化,UI 不显示)。
        drain_active_turn_inputs(false);
        maybe_inject_goal_steering();

        // One provider lease per iteration: prompt facts and chat use the
        // same snapshot. The null-provider decision remains before StepStart.
        std::shared_ptr<LlmProvider> provider_snapshot;
        if (provider_accessor_) provider_snapshot = provider_accessor_();
        // Phase 2: Build API request messages
        auto bundle = build_api_request_messages(provider_snapshot, emergency_request_profile);
        publish_side_question_context(bundle.messages_with_system);
        current_request_model_tool_names_.clear();
        current_request_model_tool_names_.reserve(bundle.tool_defs.size());
        for (const auto& def : bundle.tool_defs) {
            current_request_model_tool_names_.push_back(def.name);
        }

        // Check the same snapshot after publishing the detached context.
        if (!provider_snapshot) {
            LOG_ERROR("provider_accessor returned null; aborting turn");
            turn_timing_status = "error";
            dispatch_message(
                "error",
                no_model_config_prompt_.empty()
                    ? kDefaultNoModelConfiguredPrompt
                    : no_model_config_prompt_,
                false);
            stop_active_goal_after_turn_error(ProviderErrorInfo{});
            break;
        }

        // Phase 3: Call provider and collect response.这两个显式 lifecycle 事件
        // 是完成态 JSONL 的可靠边界;progress/usage 都不能替代它们。
        const int current_model_step = ++model_step_index;
        model_steps_->start(current_model_step);
        model_steps_->request(
            session_manager_, current_model_step, provider_snapshot, bundle, context_window());
        auto provider_result = call_provider_and_collect(
            provider_snapshot, bundle, emit_agent_progress,
            current_model_step);
        TokenUsage step_usage = provider_result.accumulated.usage;

        if (abort_signal_.raw()) {
            const auto& output = provider_result.accumulated;
            if (!output.content.empty() ||
                (output.content_parts.is_array() && !output.content_parts.empty())) {
                // Keep already displayed output across history reloads. An
                // interrupted response (especially partial tool calls) is not
                // a completed provider message, so it remains transcript-only.
                ChatMessage partial;
                partial.role = "assistant";
                partial.content = output.content;
                partial.content_parts = output.content_parts;
                partial.reasoning_content = output.reasoning_content;
                partial.metadata = {{"transcript_only", true}, {"interrupted_output", true}};
                if (session_manager_) session_manager_->on_message(partial);
                dispatch_message(partial.role, partial.content, false,
                                 partial.metadata, partial.content_parts);
            }
            model_steps_->response(session_manager_, 
                current_model_step, provider_result, step_usage, "aborted");
            model_steps_->finish(current_model_step, "aborted", step_usage);
            break;
        }

        // Phase 4: Handle provider errors (context rescue, fatal errors)
        auto error_result = handle_provider_error(
            provider_result, bundle.messages_with_system,
            turn_timing_status);
        reset_doom_guard_after_compact();
        if (error_result == HandleErrorResult::Continue) {
            model_steps_->response(session_manager_, 
                current_model_step, provider_result, step_usage, "retry");
            model_steps_->finish(current_model_step, "retry", step_usage);
            --total_iterations;
            continue;
        }
        if (error_result == HandleErrorResult::Break) {
            model_steps_->response(session_manager_, 
                current_model_step, provider_result, step_usage, "error");
            model_steps_->finish(current_model_step, "error", step_usage);
            break;
        }

        // Usage estimation when provider didn't report usage。必须覆盖所有轮:
        // 旧条件把「纯工具调用轮(无正文)」排除,导致不上报 usage 的
        // provider 下 goal 预算在工具轮从不入账,budget_limited 永不触发。
        if (!provider_result.accumulated.usage.has_data) {
            step_usage = usage_accountant_->estimate(
                *turn_usage_, provider_result.accumulated, bundle, session_manager_);
        }
        model_steps_->response(session_manager_, 
            current_model_step, provider_result, step_usage, "completed");

        // Text-only response (no tool calls) → end the loop
        if (!provider_result.accumulated.has_tool_calls()) {
            // 文本工具调用被拒:必须放在 response_is_blank 判断之前 —— provider
            // 藏起标记后内容往往只剩空白,会被误判成空回复(错误的纠正文案)。
            const TextToolCallDiagnostic& text_diag =
                provider_result.accumulated.text_tool_calls;
            if (text_diag.outcome == TextToolCallDiagnostic::Outcome::Rejected) {
                TextToolCallDiagnostic diag = text_diag;
                if (diag.reason == "truncated" &&
                    provider_result.accumulated.finish_reason == "length") {
                    diag.reason = "truncated_by_length";
                }
                const int limit = diag.format == "dsml"
                    ? kMaxDsmlToolCallCorrections
                    : kMaxTextToolCallCorrections;

                // 被拒的 assistant 消息去掉标记后落盘(原始标记只进 metadata /
                // 日志 / trajectory);只剩空白时清成空串。
                ChatMessage rejected_msg;
                rejected_msg.role = "assistant";
                rejected_msg.content = text_tool_call_rejected_persisted_content(
                    provider_result.accumulated.content, diag);
                rejected_msg.reasoning_content =
                    provider_result.accumulated.reasoning_content;
                rejected_msg.metadata = nlohmann::json{
                    {"text_tool_call_rejected", text_tool_call_diagnostic_to_json(diag)},
                };
                history_->append(rejected_msg);
                if (session_manager_) session_manager_->on_message(rejected_msg);
                if (!rejected_msg.content.empty()) {
                    // 定稿消息替换流式草稿:可疑级已经流出的标记在界面上随之消失。
                    dispatch_message("assistant", rejected_msg.content, false);
                }

                if (text_tool_call_corrections < limit) {
                    ++text_tool_call_corrections;
                    LOG_WARN("Text-form tool call rejected (format=" + diag.format +
                             " reason=" + diag.reason + "); correction " +
                             std::to_string(text_tool_call_corrections) + "/" +
                             std::to_string(limit) + ": " + diag.error +
                             " excerpt=" + log_truncate(diag.raw_excerpt, 300));

                    // 与空回复重试同款注入:role=user + hidden_goal_context,
                    // 进 API、持久化,TUI / Web 不显示。工具名取本次请求实际
                    // 发给模型的模型侧名。
                    ChatMessage correction;
                    correction.role = "user";
                    correction.content = build_text_tool_call_correction_prompt(
                        diag, current_request_model_tool_names_);
                    correction.metadata = nlohmann::json{
                        {"hidden_goal_context", true},
                        {"text_tool_call_correction", true},
                    };
                    ensure_user_message_identity(correction);
                    history_->append(correction);
                    if (session_manager_) session_manager_->on_message(correction);

                    emit_transcript_system_message(
                        std::string(u8"[文本工具调用] 模型把工具调用写成了正文文本,未执行(") +
                            diag.error + u8"),已要求其改用原生工具调用重发 " +
                            std::to_string(text_tool_call_corrections) + "/" +
                            std::to_string(limit) + u8"…",
                        make_system_notice_metadata("response_text_tool_call_retry",
                            {{"attempt", text_tool_call_corrections},
                             {"attempts", limit},
                             {"error", diag.error}}));

                    if (total_iterations > 0) {
                        --total_iterations; // 纠正轮不计入 max_iterations
                    }
                    model_steps_->finish(
                        current_model_step, "text_tool_call_retry", step_usage);
                    continue;
                }

                LOG_ERROR("Text-form tool call still rejected after " +
                          std::to_string(limit) + " correction(s); ending turn "
                          "with error (format=" + diag.format + " reason=" +
                          diag.reason + "): " + diag.error);
                turn_timing_status = "error";
                dispatch_message(
                    "error",
                    std::string(u8"[Error] 模型连续 ") + std::to_string(limit + 1) +
                        u8" 次把工具调用写成正文文本,无法执行(最后一次:" +
                        diag.error +
                        u8")。任务未完成,请重试或换用支持原生工具调用的模型。",
                    false);
                stop_active_goal_after_turn_error(ProviderErrorInfo{});
                model_steps_->finish(current_model_step, "error", step_usage);
                break;
            }

            const bool has_content_parts =
                provider_result.accumulated.content_parts.is_array() &&
                !provider_result.accumulated.content_parts.empty();
            const bool response_is_blank =
                !has_content_parts &&
                provider_result.accumulated.content.find_first_not_of(" \t\r\n") ==
                    std::string::npos;
            const bool truncated_by_length =
                provider_result.accumulated.finish_reason == "length";

            if (response_is_blank) {
                // 「成功但空」的回复是异常,不能当正常 text-only 终止。空 assistant
                // 消息仍然入历史:reasoning 回传能让模型看到自己上一轮的思考直接续
                // 上,同时给事后诊断留证据。不 dispatch 到实时流,避免空气泡。
                ChatMessage empty_msg;
                empty_msg.role = "assistant";
                empty_msg.content = provider_result.accumulated.content;
                empty_msg.reasoning_content =
                    provider_result.accumulated.reasoning_content;
                history_->append(empty_msg);
                if (session_manager_) session_manager_->on_message(empty_msg);

                if (empty_response_retries < kMaxEmptyResponseRetries) {
                    ++empty_response_retries;
                    LOG_WARN("Empty assistant response (no content, no tool_calls); "
                             "retrying " + std::to_string(empty_response_retries) +
                             "/" + std::to_string(kMaxEmptyResponseRetries) +
                             " finish_reason=" +
                             provider_result.accumulated.finish_reason +
                             " reasoning_bytes=" +
                             std::to_string(
                                 provider_result.accumulated.reasoning_content.size()));

                    // 与 stop-hook continuation 同款注入机制:role=user +
                    // hidden_goal_context,进 API、持久化,但 TUI/Web 不显示。
                    ChatMessage nudge;
                    nudge.role = "user";
                    nudge.content = truncated_by_length
                        ? "[SYSTEM NOTE] Your previous reply was cut off by the "
                          "output token limit (finish_reason=length) before any "
                          "answer text or tool call was produced. Keep internal "
                          "reasoning brief this time and continue the task now: "
                          "either call the next tool or reply with your answer "
                          "text directly."
                        : "[SYSTEM NOTE] Your previous reply was empty: it "
                          "contained no answer text and no tool calls. Continue "
                          "the task now: either call the next tool or reply with "
                          "your answer text directly.";
                    nudge.metadata = nlohmann::json{
                        {"hidden_goal_context", true},
                        {"empty_response_retry", true},
                    };
                    ensure_user_message_identity(nudge);
                    history_->append(nudge);
                    if (session_manager_) session_manager_->on_message(nudge);

                    emit_transcript_system_message(
                        std::string(u8"[空回复] 模型返回了空回复(") +
                        (truncated_by_length
                             ? u8"输出被 token 上限截断,思考耗尽了输出预算"
                             : u8"无正文也无工具调用") +
                        u8"),自动重试 " +
                        std::to_string(empty_response_retries) + "/" +
                        std::to_string(kMaxEmptyResponseRetries) + u8"…",
                        make_system_notice_metadata("response_empty_retry",
                            {{"attempt", empty_response_retries}, {"attempts", kMaxEmptyResponseRetries},
                             {"truncated", truncated_by_length}}));

                    if (total_iterations > 0) {
                        --total_iterations; // 空轮不计入 max_iterations
                    }
                    model_steps_->finish(
                        current_model_step, "empty_response_retry", step_usage);
                    continue;
                }

                LOG_ERROR("Empty assistant response persisted after " +
                          std::to_string(kMaxEmptyResponseRetries) +
                          " retries; ending turn with error");
                turn_timing_status = "error";
                dispatch_message(
                    "error",
                    std::string(u8"[Error] 模型连续 ") +
                        std::to_string(kMaxEmptyResponseRetries + 1) +
                        u8" 次返回空回复(无正文也无工具调用" +
                        (truncated_by_length
                             ? std::string(u8",输出被 token 上限截断")
                             : std::string{}) +
                        u8")。任务未完成,请重试或换用其它模型。",
                    false);
                stop_active_goal_after_turn_error(ProviderErrorInfo{});
                model_steps_->finish(current_model_step, "error", step_usage);
                break;
            }

            LOG_INFO("Text-only response; ending loop. content: " + log_truncate(provider_result.accumulated.content, 300));
            ChatMessage assistant_msg;
            assistant_msg.role = "assistant";
            assistant_msg.content = provider_result.accumulated.content;
            if (provider_result.accumulated.content_parts.is_array() && !provider_result.accumulated.content_parts.empty()) {
                assistant_msg.content_parts = provider_result.accumulated.content_parts;
            }
            assistant_msg.reasoning_content = provider_result.accumulated.reasoning_content;
            history_->append(assistant_msg);
            if (session_manager_) session_manager_->on_message(assistant_msg);
            auto completed_context = bundle.messages_with_system;
            completed_context.push_back(assistant_msg);
            publish_side_question_context(completed_context);
            {
                // 工具前言:模型若违规给最终回答也打了 <text_preamble> 标签,界面
                // 照样剥掉;落盘正文保留原文。
                const std::string visible_content =
                    llm::strip_text_preamble_tags(
                        provider_result.accumulated.content);
                const bool has_parts =
                    provider_result.accumulated.content_parts.is_array() &&
                    !provider_result.accumulated.content_parts.empty();
                if (!visible_content.empty() || has_parts) {
                    dispatch_message("assistant", visible_content, false,
                                     nlohmann::json::object(),
                                     provider_result.accumulated.content_parts);
                }
            }
            model_steps_->finish(
                current_model_step, provider_result.accumulated.finish_reason,
                step_usage);
            if (truncated_by_length) {
                emit_transcript_system_message(
                    u8"[输出截断] 本回复因输出 token 上限被截断,内容可能不完整。",
                    make_system_notice_metadata("response_truncated"));
            }
            dispatch_assistant_completed_hook(assistant_msg, provider_snapshot);
            if (maybe_continue_from_stop_hook(provider_result.accumulated.content)) {
                continue;
            }
            if (drain_active_turn_inputs(true)) {
                continue;
            }
            break;
        }

        // 本轮产出了有效输出(工具调用),连续空回复 / 文本调用纠正计数清零。
        empty_response_retries = 0;
        text_tool_call_corrections = 0;

        // 工具前言(add-tool-preamble):在 assistant(tool_calls) 消息落盘之前把
        // 本步沿用的阶段前言定下来,execute_tool_calls 开头把它挂进 metadata
        // 并随 tool_start 下发。
        current_step_preamble_ = activity_->resolve_step(provider_result.accumulated);

        // Phase 5: Execute tool calls
        terminator_fired = execute_tool_calls(
            provider_result.accumulated, provider_snapshot,
            emit_agent_progress, doom_guard, current_step_preamble_);
        // 混合形态:同一回复里既有原生调用,又有与之不一致的文本调用(回显在
        // provider 那边已剔除,记为 None 不会走到这里)。只执行了原生调用,
        // 批次跑完后追加隐藏说明,免得模型以为文本里那几个也执行了。不消耗
        // 纠正预算,不发界面通知。
        if (provider_result.accumulated.text_tool_calls.outcome ==
                TextToolCallDiagnostic::Outcome::IgnoredWithNative &&
            !terminator_fired && !abort_signal_.raw()) {
            const std::string note = build_text_tool_call_ignored_note(
                provider_result.accumulated.text_tool_calls);
            if (!note.empty()) {
                ChatMessage ignored;
                ignored.role = "user";
                ignored.content = note;
                ignored.metadata = nlohmann::json{
                    {"hidden_goal_context", true},
                    {"text_tool_call_ignored", true},
                };
                ensure_user_message_identity(ignored);
                history_->append(ignored);
                if (session_manager_) session_manager_->on_message(ignored);
            }
        }
        model_steps_->finish(
            current_model_step, provider_result.accumulated.finish_reason,
            step_usage);
        if (!terminate_session_after_turn_ && terminator_fired &&
            maybe_continue_from_stop_hook(provider_result.accumulated.content)) {
            terminator_fired = false;
            continue;
        }
        if (!terminate_session_after_turn_ && terminator_fired &&
            drain_active_turn_inputs(true)) {
            terminator_fired = false;
            continue;
        }
    }

    // Post-loop cleanup
    if (!abort_signal_.raw() && !terminator_fired &&
        has_max_iterations && total_iterations >= max_iter) {
        std::string stop_msg = "Agent loop stopped: reached max_iterations (" +
                               std::to_string(max_iter) + ")";
        LOG_WARN(stop_msg);
        turn_timing_status = "error";
        dispatch_message("system", stop_msg, false,
            make_system_notice_metadata("iteration_limit", {{"limit", max_iter}}));
        {
            // 走的是 system 角色,dispatch_message 的 error 收集点抓不到;
            // 子会话被 cap 截断时父会话同样要拿到原因。
            turn_outcome_->set_error(stop_msg);
        }
    }

    const bool interrupted_for_new_turn =
        abort_signal_.raw().load() && turn_interrupt_requested_.exchange(false);
    if (abort_signal_.raw()) {
        turn_timing_status = "aborted";
        account_goal_usage(0, false);
        if (interrupted_for_new_turn) {
            append_interrupted_turn_context(turn_info.active_turn_id);
        } else if (session_manager_) {
            const std::string sid = session_manager_->current_session_id();
            ThreadGoalStore* store = session_manager_->goal_store();
            if (store && !sid.empty()) {
                std::string error;
                if (store->pause_active_thread_goal(sid, &error)) {
                    auto goal = store->get_thread_goal(sid);
                    if (goal.has_value()) emit_goal_updated(*goal);
                }
            }
        }
    } else {
        account_goal_usage(0, false);
    }

    if (turn_info.visible_timed_turn && session_manager_) {
        auto turn_diff = session_manager_->finalize_user_turn_net_diff(
            turn_info.turn_user_uuid);
        if (turn_diff.has_value()) {
            events_.emit(SessionEventKind::TurnDiff,
                         encode_turn_net_diff(*turn_diff));
        }
    }

    if (turn_info.visible_timed_turn) {
        append_turn_timing_record(
            turn_info.turn_user_uuid, turn_info.turn_started_at_ms, now_epoch_ms(),
            turn_timing_status);
    }

    if (abort_signal_.raw()) {
        if (interrupted_for_new_turn) {
            dispatch_message("system", "[Interjected]", false,
                make_system_notice_metadata("turn_interjected", {}, {{"turn_interrupt", true}}));
        } else {
            const auto* user = trailing_transcript_message(history_->view(), true);
            // Persist the completed stop, including its exact retry target.
            // This notice stays out of the provider's message history.
            emit_transcript_system_message("[Interrupted]", make_system_notice_metadata("turn_interrupted", {}, {
                {"user_aborted", true},
                {"retry_user_message_id", user ? user->uuid : std::string{}},
            }));
        }
    }

    // 回合结束:阶段前言不留到下一回合。
    activity_->reset_turn();
    desktop_turn_lease.release_before_terminal();
    if (callbacks_.on_turn_finished) {
        callbacks_.on_turn_finished(turn_timing_status);
    }
    const auto usage = model_step_usage_to_json(turn_usage_->aggregate);
    const nlohmann::json idle = {
        {"busy", false},
        {"outcome", turn_timing_status},
        {"turn_id", turn_info.active_turn_id},
        {"usage", usage},
    };
    const nlohmann::json done = {
        {"outcome", turn_timing_status},
        {"turn_id", turn_info.active_turn_id},
        {"usage", usage},
    };
    record_terminal_trajectory_events(idle, done);
    if (callbacks_.on_busy_changed) {
        callbacks_.on_busy_changed(false);
    }
    const std::size_t dropped_steers = close_active_turn_and_discard();
    if (dropped_steers > 0) {
        LOG_WARN("[turn/steer] discarded " + std::to_string(dropped_steers) +
                 " uncommitted input(s) while closing turn " +
                 turn_info.active_turn_id);
    }
    record_turn_outcome(turn_timing_status);
    busy_ = false;
    events_.emit(SessionEventKind::BusyChanged, idle);
    events_.emit(SessionEventKind::Done, done);
    if (terminate_session_after_turn_) {
        // There must be no provider-visible state left for a deleted session.
        // The post-turn action owns writer teardown and persistent cleanup.
        history_->clear();
        auto actions = std::move(post_turn_actions_);
        post_turn_actions_.clear();
        for (auto& action : actions) {
            if (!action) continue;
            try {
                action();
            } catch (const std::exception& e) {
                LOG_ERROR(std::string("Post-turn terminal action failed: ") +
                          e.what());
            } catch (...) {
                LOG_ERROR("Post-turn terminal action failed with unknown exception");
            }
        }
    } else {
        maybe_continue_goal();
    }
}

} // namespace acecode
