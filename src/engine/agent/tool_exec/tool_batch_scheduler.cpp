#include "agent/agent_loop.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/event_payload/message_payload.hpp"
#include "agent/guards/doom_guard.hpp"
#include "agent/tool_exec/tool_batch_types.hpp"
#include "computer_use/runtime.hpp"
#include "hooks/hook_manager.hpp"
#include "hooks/hook_runtime.hpp"
#include "llm/tool_protocol_names.hpp"
#include "pa/pa_overflow_rescue.hpp"
#include "permissions/interaction_mode.hpp"
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
#include "session/tool_metadata_codec.hpp"
#include "session/tool_result_storage.hpp"
#include "session/turn_timing.hpp"
#include "skills/skill_usage_store.hpp"
#include "tool/mtime_tracker.hpp"
#include "llm/text_preamble_tags.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"
#include "utils/text.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>
#include <future>
#include <thread>

namespace acecode {

bool AgentLoop::execute_tool_calls(
    const ChatResponse& accumulated,
    const std::shared_ptr<LlmProvider>& provider_snapshot,
    const ProgressEmitter& emit_progress,
    agent::SynchronizedDoomGuard& doom_guard,
    ToolPreambleTitle& pending_preamble) {
    // Record the assistant message with tool_calls in the history
    auto tc_msg = ToolExecutor::format_assistant_tool_calls(accumulated);
    // 文本工具调用恢复成功:消息本体与原生调用字节级同形(不会发给模型的
    // metadata 只多一个诊断字段),让后续历史里出现原生调用可供模仿。
    if (accumulated.text_tool_calls.outcome ==
        TextToolCallDiagnostic::Outcome::Recovered) {
        if (!tc_msg.metadata.is_object()) tc_msg.metadata = nlohmann::json::object();
        tc_msg.metadata["text_tool_call_recovery"] = {
            {"format", accumulated.text_tool_calls.format},
            {"count", accumulated.text_tool_calls.recovered_count > 0
                          ? accumulated.text_tool_calls.recovered_count
                          : static_cast<int>(accumulated.tool_calls.size())},
        };
    }
    // 工具前言(add-tool-preamble):本批次沿用的阶段前言挂在这条
    // assistant(tool_calls) 消息的 metadata 上落盘(只为记录,不还原任何显示行),
    // 实时界面经每个调用的 tool_start.preamble 拿到它。
    const ToolPreambleTitle step_preamble = std::move(pending_preamble);
    pending_preamble = {};
    nlohmann::json preamble_metadata;
    if (!step_preamble.title.empty()) {
        preamble_metadata = {
            {"source", step_preamble.source},
            {"title", step_preamble.title},
            {"kind", step_preamble.kind},
        };
        if (!tc_msg.metadata.is_object()) tc_msg.metadata = nlohmann::json::object();
        tc_msg.metadata[agent::kToolPreambleMetadataKey] = preamble_metadata;
    }
    // 单个调用的前言 = 本批次的阶段前言。
    history_->append(tc_msg);
    if (session_manager_) session_manager_->on_message(tc_msg);
    dispatch_assistant_completed_hook(tc_msg, provider_snapshot);

    // Web: 工具调用回合的 assistant 文本此前只通过 token 流下发,没有一条权威的
    // Message 帧。文本-only 回合靠末尾那条 assistant Message 事件整体替换流式草稿
    // 来兜底(见 run_agent 的 text-only 分支),工具回合缺这一步 —— 一旦流式 token
    // 在传输/竞态中丢了一段,前端草稿就停在半截,且因为没有权威帧,生成结束也无法
    // 自愈(磁盘已落全量,所以切会话重载才恢复)。这里补发一条 assistant 文本的
    // Message 事件让前端用完整文本整体替换草稿。仅走 web 的 events_,不经
    // dispatch_message 的 on_message 回调,避免改变 TUI 的流式渲染行为。
    // 工具前言:正文里的 <text_preamble> 标签不进界面(id 仍按落盘原文算,与
    // GET /messages 重读一致);整段都是标签时不发这条帧。
    const std::string visible_content =
        llm::strip_text_preamble_tags(accumulated.content);
    const bool has_content_parts =
        accumulated.content_parts.is_array() && !accumulated.content_parts.empty();
    if (!visible_content.empty() || has_content_parts) {
        ChatMessage id_basis;
        id_basis.role = "assistant";
        id_basis.content = accumulated.content;
        nlohmann::json assistant_event = {
            {"role", "assistant"},
            {"content", visible_content},
            {"is_tool", false},
            {"id", web::compute_message_id(id_basis)},
        };
        if (has_content_parts) {
            assistant_event["content_parts"] = accumulated.content_parts;
        }
        if (preamble_metadata.is_object()) {
            assistant_event["metadata"] = {
                {agent::kToolPreambleMetadataKey, preamble_metadata}};
        }
        events_.emit(SessionEventKind::Message, std::move(assistant_event));
    }

    // Partition tool calls into read-only (parallelizable) and write (serial) groups
    LOG_INFO("Processing " + std::to_string(accumulated.tool_calls.size()) + " tool calls");

    struct ToolCallEntry {
        size_t original_index;
        const ToolCall* tc;
    };

    std::vector<ToolCallEntry> read_entries, write_entries;
    for (size_t i = 0; i < accumulated.tool_calls.size(); ++i) {
        const auto& tc = accumulated.tool_calls[i];
        ToolCallEntry entry{i, &tc};
        if (tools_.can_execute_in_parallel(tc.function_name)) {
            read_entries.push_back(entry);
        } else {
            write_entries.push_back(entry);
        }
    }

    LOG_INFO("Partitioned: " + std::to_string(read_entries.size()) + " read-only, " +
             std::to_string(write_entries.size()) + " write");

    // Results array indexed by original position
    std::vector<ToolResult> results(accumulated.tool_calls.size());
    std::vector<bool> result_ready(accumulated.tool_calls.size(), false);
    // Each parallel tool writes only its own slot. Collect after joining so
    // early delivery replacements keep the existing durable replacement audit.
    std::vector<ToolResultReplacementRecord> delivery_replacements(
        accumulated.tool_calls.size());

    std::vector<DeferredTaskCompleteEnd> deferred_task_complete_ends(
        accumulated.tool_calls.size());
    ToolBatchState batch{
        doom_guard, emit_progress, step_preamble,
        delivery_replacements, deferred_task_complete_ends};

    // Helper: extract context from a tool call

    // boundary_root = write_root():非空即"有写边界"(worktree / LOOP / 从父
    // 会话继承)。有边界的 Yolo 会话只豁免只读工具,写工具必须过边界校验;
    // 无边界的 Yolo 会话维持旧行为(全部豁免)。曾经只有 LOOP 主会话有这条
    // 边界,spawn_subagent 派生的子会话继承 Yolo 却不继承 LOOP 身份,于是在
    // worktree 里起的子代理可以随手把改动写进主 checkout。

    // Helper: execute a single tool (for both parallel and serial use).

    // 展示层的结果行派发(tool_result 伪行 + on_tool_result 补挂 summary/
    // hunks)。从 Phase 3 前移到各执行点,让「调用行 → 结果行」成对相邻出现
    // 而不是先挤一排调用再挤一排结果。单个大结果已在 lifecycle 内落盘并
    // 替换为文件引用,避免 live 事件与 TUI 再保留全文;结构化 hunks 保留。
    // canonical 落盘与跨结果的 aggregate budget 仍在 Phase 3 统一进行。

    // Phase 1: Execute read-only tools in parallel
    if (!read_entries.empty() && !abort_signal_.raw()) {
        unsigned int max_concurrency = std::min(
            static_cast<unsigned int>(4),
            std::max(static_cast<unsigned int>(1), std::thread::hardware_concurrency()));

        struct PendingReadTool {
            size_t original_index;
            ToolCall call;
            std::future<ToolResult> future;
        };

        size_t i = 0;
        while (i < read_entries.size() && !abort_signal_.raw()) {
            size_t batch_end = std::min(i + max_concurrency, read_entries.size());
            std::vector<PendingReadTool> pending;

            for (size_t j = i; j < batch_end; ++j) {
                const auto& entry = read_entries[j];
                ToolCall tc_copy = *entry.tc;
                size_t original_index = entry.original_index;
                pending.push_back(PendingReadTool{
                    original_index,
                    tc_copy,
                    std::async(std::launch::async,
                    [this, &batch, tc_copy, original_index]() {
                        return run_tool_with_lifecycle(
                            batch, tc_copy, original_index, false,
                            [this, &batch](
                                 const ToolCall& effective_tc,
                                 const ToolContext& ctx,
                                 const std::string& ctx_path,
                                 const std::string&) {
                                const ToolCapabilityPolicy* policy =
                                    ctx.capability_policy
                                        ? &*ctx.capability_policy
                                        : nullptr;
                                if (tools_.is_denied_by_policy(
                                        effective_tc.function_name, policy)) {
                                    return ToolResult{
                                        "[Error] Tool denied by the active "
                                        "expert capability policy: " +
                                            effective_tc.function_name,
                                        false};
                                }
                                if (auto guarded = maybe_guard_tool(batch, effective_tc)) {
                                    return *guarded;
                                }
                                return execute_single_tool(
                                    effective_tc.function_name, effective_tc.function_arguments,
                                    ctx_path, ctx);
                            });
                    })
                });
            }

            for (auto& item : pending) {
                size_t idx = item.original_index;
                // 展示层成对派发:调用行在阻塞等待它的结果之前亮出(执行中
                // 灰色指示灯),结果一到紧跟其后 —— 即使批内并行执行,transcript
                // 仍按提交顺序呈现「调用 → 结果」相邻的成对行。abort 时未收割
                // 的调用不再显示伪行,canonical 的 [Interrupted] 由 Phase 3 落盘。
                dispatch_message("tool_call",
                    "[Tool: " + item.call.function_name + "] " +
                        item.call.function_arguments, true);
                try {
                    results[idx] = item.future.get();
                } catch (const std::exception& e) {
                    results[idx] = ToolResult{"[Error] " + std::string(e.what()), false};
                    ensure_tool_summary(
                        item.call.function_name,
                        item.call.function_arguments,
                        results[idx]);
                }
                result_ready[idx] = true;
                record_doom_guard_result(batch, item.call, results[idx]);
                account_goal_usage(0, false);
                dispatch_tool_result_display(item.call, results[idx]);
            }

            i = batch_end;
        }
    }

    // Phase 2: Execute write tools sequentially (with permission checks)
    for (const auto& entry : write_entries) {
        if (abort_signal_.raw()) break;

        const auto& tc = *entry.tc;
        LOG_INFO("Tool call (write): " + tc.function_name + " id=" + tc.id);

        dispatch_message("tool_call",
                "[Tool: " + tc.function_name + "] " + tc.function_arguments, true);

        results[entry.original_index] = run_tool_with_lifecycle(
            batch, tc, entry.original_index, true,
            [this, &batch, tool_index = entry.original_index](
                const ToolCall& effective_tc,
                const ToolContext& tool_ctx,
                const std::string& ctx_path,
                const std::string& ctx_command) {
                return run_write_tool(batch, effective_tc, tool_ctx,
                                      ctx_path, ctx_command, tool_index);
            });
        result_ready[entry.original_index] = true;
        record_doom_guard_result(batch, tc, results[entry.original_index]);
        account_goal_usage(0, false);
        // 结果行紧跟派发。调用行在执行前已显示(权限确认弹窗需要上下文),
        // 写工具串行执行,顺序天然成对。
        dispatch_tool_result_display(tc, results[entry.original_index]);
        if (results[entry.original_index].terminate_session_after_turn) {
            LOG_INFO("Stopping remaining write tools after terminal session action");
            break;
        }
    }

    std::vector<ToolResultReplacementRecord> replacement_records;
    for (size_t i = 0; i < delivery_replacements.size(); ++i) {
        if (result_ready[i] && !delivery_replacements[i].tool_call_id.empty()) {
            replacement_records.push_back(std::move(delivery_replacements[i]));
        }
    }
    if (session_manager_) {
        const std::string tool_results_dir = session_manager_->ensure_tool_results_dir();
        if (!tool_results_dir.empty()) {
            auto replacement_state = reconstruct_tool_result_replacement_state(history_->view());
            auto budget_result = enforce_tool_result_budget(
                accumulated.tool_calls,
                results,
                result_ready,
                tool_results_dir,
                replacement_state);
            for (auto& record : budget_result.newly_replaced) {
                replacement_records.push_back(std::move(record));
            }
        }
    }

    auto record_file_read_result_reference = [](const ToolCall& tc, const ToolResult& result) {
        if (!result.success || tc.function_name != "file_read") return;
        if (result.output.rfind("File unchanged since last read.", 0) == 0) return;

        auto args = nlohmann::json::parse(tc.function_arguments, nullptr, false);
        if (!args.is_object() ||
            !args.contains("file_path") ||
            !args["file_path"].is_string()) {
            return;
        }

        auto int_arg = [&args](const char* key) -> int {
            if (!args.contains(key) || !args[key].is_number_integer()) return 0;
            return args[key].get<int>();
        };
        auto uint64_arg = [&args](const char* key) -> uint64_t {
            if (!args.contains(key)) return 0;
            if (args[key].is_number_unsigned()) return args[key].get<uint64_t>();
            if (!args[key].is_number_integer()) return 0;
            const auto value = args[key].get<int64_t>();
            return value >= 0 ? static_cast<uint64_t>(value) : 0;
        };
        const bool byte_mode = args.contains("byte_offset");

        MtimeTracker::instance().record_read_observation_result(
            args["file_path"].get<std::string>(),
            int_arg("start_line"),
            int_arg("end_line"),
            tc.id,
            persisted_output_filepath(result.output),
            byte_mode,
            uint64_arg("byte_offset"),
            static_cast<size_t>(uint64_arg("max_bytes")));
    };

    for (size_t i = 0; i < accumulated.tool_calls.size() && i < results.size(); ++i) {
        if (i < result_ready.size() && result_ready[i]) {
            record_file_read_result_reference(accumulated.tool_calls[i], results[i]);
        }
    }

    // Phase 3: Record and dispatch all results in original order
    for (size_t i = 0; i < accumulated.tool_calls.size(); ++i) {
        const auto& tc = accumulated.tool_calls[i];
        ChatMessage tool_msg;
        if (result_ready[i]) {
            tool_msg = ToolExecutor::format_tool_result(tc.id, results[i]);
            if (results[i].summary.has_value()) {
                tool_msg.metadata["tool_summary"] = encode_tool_summary(*results[i].summary);
            }
            if (results[i].hunks.has_value()) {
                tool_msg.metadata["tool_hunks"] = encode_tool_hunks(*results[i].hunks);
            }
        } else {
            ToolResult interrupted_result{"[Interrupted]", false};
            ensure_tool_summary(
                tc.function_name, tc.function_arguments, interrupted_result);
            tool_msg = ToolExecutor::format_tool_result(
                tc.id, interrupted_result);
            // AskUserQuestion deliberately has no synthesized summary, so the
            // metadata key must stay absent instead of dereferencing nullopt.
            if (interrupted_result.summary.has_value()) {
                tool_msg.metadata["tool_summary"] =
                    encode_tool_summary(*interrupted_result.summary);
            }
        }
        history_->append(tool_msg);
        if (session_manager_) session_manager_->on_message(tool_msg);

        if (tc.function_name == "task_complete" &&
            result_ready[i] && results[i].success) {
            const DeferredTaskCompleteEnd deferred =
                i < deferred_task_complete_ends.size()
                    ? deferred_task_complete_ends[i]
                    : DeferredTaskCompleteEnd{};
            auto end_payload = web::build_tool_end_payload(
                tc.function_name, results[i], deferred.elapsed_seconds,
                results[i].output, tc.id, static_cast<int>(i),
                web::compute_message_id(tool_msg));
            if (session_manager_) {
                auto trajectory_payload = end_payload;
                trajectory_payload["started_at_ms"] = deferred.started_at_ms;
                trajectory_payload["completed_at_ms"] = deferred.completed_at_ms;
                trajectory_payload["duration_ms"] = deferred.duration_ms;
                session_manager_->record_trajectory_event(
                    "tool_end", std::move(trajectory_payload),
                    deferred.completed_at_ms);
            }
            events_.emit(
                SessionEventKind::ToolEnd, std::move(end_payload));
        }

        // 展示派发(tool_result 伪行 + on_tool_result)已前移到各执行点
        // (dispatch_tool_result_display),这里只保留 canonical 相关处理。
        if (result_ready[i]) {
            if (results[i].post_user_prompt.has_value() &&
                !results[i].post_user_prompt->empty()) {
                append_tool_user_prompt(
                    *results[i].post_user_prompt,
                    results[i].post_user_prompt_display_text,
                    tc.function_name);
            }
        }
    }

    if (!replacement_records.empty()) {
        ChatMessage meta_msg = encode_content_replacement_message(replacement_records);
        history_->append(meta_msg);
        if (session_manager_) session_manager_->on_message(meta_msg);
    }

    // A session-terminal action takes precedence over ordinary terminators.
    // Move its callback only after every canonical result has been recorded.
    for (size_t i = 0; i < accumulated.tool_calls.size(); ++i) {
        if (!result_ready[i] ||
            !results[i].terminate_session_after_turn) {
            continue;
        }
        terminate_session_after_turn_ = true;
        if (results[i].post_turn_action) {
            post_turn_actions_.push_back(
                std::move(results[i].post_turn_action));
        }
        LOG_INFO("Terminal session action queued after turn boundary");
    }
    if (terminate_session_after_turn_) return true;

    // Terminator detection. A failed ExitPlanMode is a user/runtime boundary:
    // retrying it in the same turn only replays the approval request while the
    // session correctly remains in Plan mode.
    for (size_t i = 0; i < accumulated.tool_calls.size(); ++i) {
        const auto& tc = accumulated.tool_calls[i];
        if (tc.function_name == "task_complete" && result_ready[i] && results[i].success) {
            LOG_INFO("Terminator fired: task_complete");
            return true;
        }
        if (tc.function_name == "ExitPlanMode" && result_ready[i] && !results[i].success) {
            LOG_INFO("Ending turn after failed ExitPlanMode");
            return true;
        }
    }
    return false;
}

} // namespace acecode
