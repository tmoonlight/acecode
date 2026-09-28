#include "agent/agent_loop.hpp"
#include "agent/hook_bridge/tool_hook_bridge.hpp"
#include "agent/tool_exec/tool_stream_progress.hpp"
#include "agent/approval/permission_payloads.hpp"
#include "agent/tool_exec/tool_batch_types.hpp"
#include "hooks/hook_manager.hpp"
#include "hooks/hook_runtime.hpp"
#include "permissions/shell_write_guard.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/output_attachments.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/task_suggestion_store.hpp"
#include "session/thread_goal_store.hpp"
#include "session/thread_repair.hpp"
#include "session/token_tracker.hpp"
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

using agent::detail::parse_tool_args_for_permission_payload;
using utils::now_epoch_ms;

ToolResult AgentLoop::run_tool_with_lifecycle(ToolBatchState& batch, ToolCall tc, size_t tool_index, bool emit_tui_progress, const ToolRunner& runner) {
    const auto& emit_progress = batch.emit_progress;
    const auto& step_preamble = batch.step_preamble;
    auto& delivery_replacements = batch.delivery_replacements;
    auto& deferred_task_complete_ends = batch.deferred_task_complete_ends;
    auto preamble_for_call = [&step_preamble](const ToolCall&, std::size_t) {
        return step_preamble.title;
    };
    if (auto denied = tool_hooks_->before(hook_manager_, session_manager_, tc, tool_index)) {
        return std::move(*denied);
    }

    std::string exec_path, exec_cmd;
    extract_context(tc, exec_path, exec_cmd);

    std::string cmd_preview;
    if (!exec_cmd.empty()) cmd_preview = exec_cmd;
    else if (!exec_path.empty()) cmd_preview = exec_path;
    else cmd_preview = tc.function_name;
    cmd_preview = truncate_utf8_prefix(cmd_preview, 60);

    std::string display_override =
        ToolExecutor::build_tool_call_preview(tc.function_name, tc.function_arguments);
    bool is_task_complete = (tc.function_name == "task_complete");

    auto tool_start_tp = std::chrono::steady_clock::now();
    const std::int64_t tool_started_at_ms = now_epoch_ms();
    const int tool_index_int = static_cast<int>(tool_index);
    const std::string call_preamble = preamble_for_call(tc, tool_index);

    {
        nlohmann::json args_payload;
        try { args_payload = nlohmann::json::parse(tc.function_arguments); }
        catch (...) { args_payload = tc.function_arguments; }
        auto start_payload = web::build_tool_start_payload(
            tc.function_name, args_payload,
            cmd_preview, display_override,
            is_task_complete, tc.id, tool_index_int);
        start_payload["started_at_ms"] = tool_started_at_ms;
        // 工具前言:这次调用的前言随 tool_start 下发,工具行 / loading 直接用。
        if (!call_preamble.empty()) {
            start_payload["preamble"] = call_preamble;
            start_payload["preamble_source"] = step_preamble.source;
            start_payload["preamble_kind"] = step_preamble.kind;
        }
        events_.emit(
            SessionEventKind::ToolStart, std::move(start_payload));
    }

    // 开启具体进度提示时,发射口把这条换成本批次文案、清掉命令预览。
    emit_progress("tool_running", "正在调用工具 " + tc.function_name,
        cmd_preview, tc.function_name, tc.id, tool_index_int, true);

    auto prog = std::make_shared<agent::ToolStreamProgress>();

    ToolContext tool_ctx = build_tool_context();
    // Wire up per-call callbacks that aren't in the base context
    if (ask_prompter_) {
        AskUserQuestionPrompter* p = ask_prompter_;
        std::atomic<bool>* abort_flag_ptr = &abort_signal_.flag_for_legacy_api();
        const std::string tool_name_for_question = tc.function_name;
        const std::string tool_call_id_for_question = tc.id;
        tool_ctx.ask_user_questions =
            [this, p, abort_flag_ptr, emit_progress, tool_name_for_question,
             tool_call_id_for_question, tool_index_int](const nlohmann::json& questions_payload) -> nlohmann::json {
                emit_progress("question_waiting", "正在等待用户回答",
                    std::string{}, tool_name_for_question,
                    tool_call_id_for_question, tool_index_int, true);
                std::optional<std::chrono::milliseconds> timeout_override;
                if (goal_unattended_active()) {
                    timeout_override = std::chrono::seconds(
                        kGoalQuestionTimeoutSeconds);
                }
                AskUserQuestionResponse resp = p->prompt(
                    questions_payload, abort_flag_ptr, timeout_override);
                nlohmann::json out;
                out["cancelled"] = resp.cancelled;
                // timeout 策略到期(add-ask-question-policy):工具侧据此
                // 合成「自动采纳每题第一选项」的结果。
                out["timed_out"] = resp.timed_out;
                // 用户在提问挂起时直接发文本(interject_question):
                // 工具侧据此给模型「改为直接输入,看下一条 user 消息」。
                out["interjected"] = resp.interjected;
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& a : resp.answers) {
                    nlohmann::json item;
                    item["question_id"] = a.question_id;
                    item["selected"]    = a.selected;
                    item["custom_text"] = a.custom_text;
                    // 对齐 TUI(ask_question_controller.cpp):selected 与
                    // custom_text 均为空的题视为未作答,让 Web 端「跳过」
                    // 在 LLM 结果中呈现为 "Not answered" 而非空串。
                    item["not_answered"] = a.selected.empty() && a.custom_text.empty();
                    arr.push_back(std::move(item));
                }
                out["answers"] = std::move(arr);
                return out;
            };
    }
    else if (ask_channel_) {
        // TUI 路径:同一个口子,只是传输换成 overlay 阻塞等待。
        // 超时与来源标注在这里算 —— 与 daemon 给 prompter 算
        // timeout_override 是同一处职责,两端不会各自漂移。
        AskQuestionChannel channel = ask_channel_;
        std::atomic<bool>* abort_flag_ptr = &abort_signal_.flag_for_legacy_api();
        const ResolvedQuestionPolicy policy = resolved_question_policy();
        int timeout_seconds = 0;
        if (goal_unattended_active()) {
            timeout_seconds = kGoalQuestionTimeoutSeconds;
        } else if (policy.policy == QuestionPolicy::Timeout) {
            timeout_seconds = policy.timeout_seconds;
        }
        std::string origin_label;
        if (session_manager_ &&
            !session_manager_->current_parent_session_id().empty()) {
            const std::string child_title = session_manager_->current_title();
            origin_label = "[subagent] " +
                (child_title.empty() ? session_manager_->current_session_id()
                                     : child_title);
        }
        tool_ctx.ask_user_questions =
            [channel, abort_flag_ptr, timeout_seconds, origin_label](
                const nlohmann::json& questions_payload) -> nlohmann::json {
                return channel(questions_payload, abort_flag_ptr,
                               timeout_seconds, origin_label);
            };
    }

    std::function<void(const std::vector<std::string>&,
                       const std::string&,
                       size_t,
                       int)> stream_update_cb;
    if (emit_tui_progress) stream_update_cb = callbacks_.on_tool_progress_update;
    EventDispatcher* events_ptr = &events_;
    std::string tool_name_copy = tc.function_name;
    std::string tool_call_id_copy = tc.id;
    const std::string update_coalesce_key = "tool_update:" +
        (!tc.id.empty() ? tc.id : (tc.function_name + ":" + std::to_string(tool_index_int)));
    // P0-11:500ms 工具输出帧的取时走本回合捕获的时钟快照(默认 steady_clock)。
    const SteadyClockFn stream_clock = turn_progress_clock_;
    tool_ctx.stream = [prog, stream_update_cb, events_ptr, tool_start_tp,
                        tool_name_copy, tool_call_id_copy, tool_index_int,
                        update_coalesce_key, stream_clock](const std::string& chunk) {
        const auto progress = prog->append(
            chunk, stream_clock ? stream_clock() : std::chrono::steady_clock::now());
        const auto& snapshot = progress.tail_lines;
        const auto& current_partial = progress.current_partial;
        const auto total_bytes = progress.total_bytes;
        const auto total_lines = progress.total_lines;
        const bool should_emit = progress.should_emit;
        if (stream_update_cb) {
            stream_update_cb(snapshot, current_partial, total_bytes, total_lines);
        }
        if (!should_emit) return;
        auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tool_start_tp).count();
        EventDispatcher::EmitOptions opts;
        opts.buffered = true;
        opts.coalesce_key = update_coalesce_key;
        events_ptr->emit(SessionEventKind::ToolUpdate,
            web::build_tool_update_payload(tool_name_copy, snapshot,
                                             current_partial,
                                             total_lines,
                                             total_bytes,
                                             elapsed_ms / 1000.0,
                                             tool_call_id_copy,
                                             tool_index_int),
            opts);
    };

    struct ProgressGuard {
        std::function<void()> end_cb;
        ~ProgressGuard() { if (end_cb) end_cb(); }
    };
    ProgressGuard guard;
    if (emit_tui_progress && callbacks_.on_tool_progress_start) {
        callbacks_.on_tool_progress_start(tc.function_name, cmd_preview, std::string{});
        guard.end_cb = callbacks_.on_tool_progress_end;
    }

    ToolResult result;
    try {
        result = runner(tc, tool_ctx, exec_path, exec_cmd);
    } catch (const std::exception& e) {
        LOG_ERROR("Tool lifecycle runner error: " + std::string(e.what()));
        result = ToolResult{"[Error] Tool execution failed: " + std::string(e.what()), false};
    }
    tool_hooks_->after(hook_manager_, session_manager_, tc, result);
    materialize_result_attachments(result);
    mark_workspace_scratch_change(result, tool_ctx);
    if (session_manager_) {
        // Both ToolEnd and the following tool_result Message are sent live.
        // Persist before either can retain a full output in replay/UI state.
        // PostToolUse has already seen its original input; preserve hunks
        // and other structured fields for specialized file-diff rendering.
        if (prepare_tool_result_for_delivery(
                result, tc.function_name, tc.id,
                session_manager_->ensure_tool_results_dir()) && !tc.id.empty()) {
            delivery_replacements[tool_index] = {tc.id, result.output};
        }
    }
    ensure_tool_summary(
        tc.function_name, tc.function_arguments, result);

    auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tool_start_tp).count();
    const std::int64_t tool_completed_at_ms = now_epoch_ms();
    std::string snippet;
    if (!result.success) {
        int lines = 0;
        for (char c : result.output) {
            snippet.push_back(c);
            if (c == '\n' && ++lines >= 20) break;
        }
    }
    const bool defer_task_complete_end = is_task_complete && result.success;
    if (defer_task_complete_end &&
        tool_index < deferred_task_complete_ends.size()) {
        auto& deferred = deferred_task_complete_ends[tool_index];
        deferred.started_at_ms = tool_started_at_ms;
        deferred.completed_at_ms = tool_completed_at_ms;
        deferred.duration_ms = elapsed_ms;
        deferred.elapsed_seconds = elapsed_ms / 1000.0;
    }
    if (session_manager_ && !defer_task_complete_end) {
        auto trajectory_payload = web::build_tool_end_payload(
            tc.function_name, result,
            elapsed_ms / 1000.0,
            result.output,
            tc.id, tool_index_int);
        trajectory_payload["started_at_ms"] = tool_started_at_ms;
        trajectory_payload["completed_at_ms"] = tool_completed_at_ms;
        trajectory_payload["duration_ms"] = elapsed_ms;
        session_manager_->record_trajectory_event(
            "tool_end", std::move(trajectory_payload),
            tool_completed_at_ms);
    }
    if (defer_task_complete_end) {
        // task_complete 的 fork 边界必须指向预算替换后实际落盘的
        // canonical tool-result。延迟 trajectory/live ToolEnd 到 Phase 3
        // 持久化之后,避免超长 summary 的预计算 ID 与 REST/fork 不一致。
    } else {
        events_.emit(
            SessionEventKind::ToolEnd,
            web::build_tool_end_payload(
                tc.function_name, result,
                elapsed_ms / 1000.0, snippet,
                tc.id, tool_index_int));
    }
    return result;
}

} // namespace acecode
