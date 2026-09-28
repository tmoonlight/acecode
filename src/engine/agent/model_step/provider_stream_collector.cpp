#include "agent/agent_loop.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/model_step/active_provider_slot.hpp"
#include "agent/approval/permission_payloads.hpp"
#include "agent/compaction/compact.hpp"
#include "agent/detail/agent_payloads.hpp"
#include "agent/guards/doom_guard.hpp"
#include "agent/progress/retry_progress.hpp"
#include "agent/request/request_context.hpp"
#include "agent/transcript/transcript_queries.hpp"
#include "llm/tool_protocol_names.hpp"
#include "pa/pa_context_budget.hpp"
#include "pa/pa_overflow_rescue.hpp"
#include "permissions/shell_write_guard.hpp"
#include "prompt/context_usage_breakdown.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/thread_goal_store.hpp"
#include "session/thread_repair.hpp"
#include "session/token_tracker.hpp"
#include "tool/mtime_tracker.hpp"
#include "tool_preamble/tool_preamble.hpp"
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

namespace acecode {

using agent::detail::model_step_usage_to_json;
using agent::detail::accumulate_turn_usage;
using agent::detail::build_transcript_replace_payload;
using agent::detail::human_bytes;
using agent::detail::format_bytes_detail;

AgentLoop::ProviderCallResult AgentLoop::call_provider_and_collect(
    const std::shared_ptr<LlmProvider>& provider,
    const ApiRequestBundle& bundle,
    const ProgressEmitter& emit_progress,
    int model_step_index) {
    ProviderCallResult result;
    result.accumulated.finish_reason = "stop";
    result.provider_snapshot = provider;

    std::mutex resp_mu;
    std::size_t reasoning_bytes = 0;
    int reasoning_fragments = 0;
    int provider_attempt = 1;
    bool first_output_recorded = false;

    // 具体进度提示(add-tool-preamble):本次调用期间的开关快照。开启时在推理流
    // 里抠第一对加粗标题作本步文案;正文开始流出时 loading 换成「正在撰写回复」。
    // 每次调用(含重试)都从干净的本步状态开始。
    const bool concrete = concrete_activity_enabled();
    bool reasoning_title_found = false;
    bool step_text_started = false;
    reset_activity_for_step();
    text_preamble_scanner_.reset();
    // 正文增量的统一出口:可见文本给 TUI / Web。
    auto publish_visible_text = [&](const std::string& text) {
        if (text.empty()) return;
        if (concrete && !step_text_started &&
            text.find_first_not_of(" \t\r\n") != std::string::npos) {
            step_text_started = true;
            emit_progress("responding", tool_preamble::kRespondingActivityLabel,
                          std::string{}, std::string{}, std::string{}, -1, true);
        }
        if (callbacks_.on_delta) {
            callbacks_.on_delta(text);
        }
        events_.emit(SessionEventKind::Token, nlohmann::json{{"text", text}});
    };
    // 历史里残留的 <text_preamble> 标签(前一版要求模型打标签)总是从可见正文里
    // 剥掉,不再当 loading 文案。
    auto publish_scanned = [&](tool_preamble::TextPreambleScanner::Output out) {
        publish_visible_text(out.visible);
    };

    auto stream_callback = [&result, &resp_mu, &emit_progress, &bundle,
                            &reasoning_bytes, &reasoning_fragments,
                            &provider_attempt, &first_output_recorded,
                            &reasoning_title_found, &step_text_started,
                            &publish_scanned,
                            model_step_index, concrete,
                            this](const StreamEvent& evt) {
        switch (evt.type) {
        case StreamEventType::Delta:
            if (!evt.content.empty() && !first_output_recorded) {
                first_output_recorded = true;
                if (session_manager_) {
                    session_manager_->record_trajectory_event(
                        "model_first_output",
                        {{"step_index", model_step_index},
                         {"attempt", provider_attempt},
                         {"channel", "content"}});
                }
            }
            {
                std::lock_guard<std::mutex> lk(resp_mu);
                // 落盘正文保留标签原文(模型会模仿自己的历史输出,剥掉历史
                // 反而让它几轮后忘记格式);只有下发给界面的 token 流是剥过的。
                result.accumulated.content += evt.content;
            }
            publish_scanned(text_preamble_scanner_.feed(evt.content));
            break;
        case StreamEventType::ReasoningDelta:
            if (!evt.content.empty() && !first_output_recorded) {
                first_output_recorded = true;
                if (session_manager_) {
                    session_manager_->record_trajectory_event(
                        "model_first_output",
                        {{"step_index", model_step_index},
                         {"attempt", provider_attempt},
                         {"channel", "reasoning"}});
                }
            }
            {
                std::lock_guard<std::mutex> lk(resp_mu);
                result.accumulated.reasoning_content += evt.content;
            }
            reasoning_bytes += evt.content.size();
            reasoning_fragments++;
            if (concrete && !reasoning_title_found) {
                std::string reasoning_so_far;
                {
                    std::lock_guard<std::mutex> lk(resp_mu);
                    reasoning_so_far = result.accumulated.reasoning_content;
                }
                const std::string bold =
                    tool_preamble::extract_first_bold_span(reasoning_so_far);
                if (!bold.empty()) {
                    const std::string title = tool_preamble::normalize_title_line(
                        bold, tool_preamble::kReasoningTitleMaxCodePoints);
                    if (!title.empty()) {
                        reasoning_title_found = true;
                        ToolPreambleTitle found;
                        found.title = title;
                        found.source = tool_preamble::kSourceReasoning;
                        publish_phase_preamble(found, emit_progress);
                    }
                }
            }
            // 开启具体进度提示时,发射口会把这条换成本步标题或场景文案。
            emit_progress("reasoning", "正在推理",
                "片段 " + std::to_string(reasoning_fragments) + ", " +
                human_bytes(reasoning_bytes),
                std::string{}, std::string{}, -1, false);
            events_.emit(SessionEventKind::Reasoning, nlohmann::json{{"text", evt.content}});
            break;
        case StreamEventType::ToolCallDelta:
            if (evt.text_tool_call_hold) {
                // provider 正在扣住一段疑似文本工具调用:只是进度提示,不代表
                // 模型已经开始输出原生调用,所以不记 model_first_output 的
                // channel(等真正的正文或调用出现时再记),也不走工具名解析 /
                // 前言抽取(tool_index=-1、工具名为空)。
                emit_progress("tool_planning", "正在准备工具调用",
                    human_bytes(evt.tool_call_argument_bytes),
                    std::string{}, std::string{}, -1, false);
                break;
            }
            if (!first_output_recorded) {
                first_output_recorded = true;
                if (session_manager_) {
                    session_manager_->record_trajectory_event(
                        "model_first_output",
                        {{"step_index", model_step_index},
                         {"attempt", provider_attempt},
                         {"channel", "tool_call"}});
                }
            }
            {
                const std::string tool_name =
                    tools_.resolve_model_tool_name_to_native(
                        evt.tool_call.function_name);
                const std::string label = tool_name.empty()
                    ? "正在准备工具调用"
                    : "正在准备调用 " + tool_name;
                // 具体进度提示:记下本步流出来的工具,发射口据此拼「正在读取 2 个文件」。
                note_planned_tool(evt.tool_index, tool_name);
                emit_progress("tool_planning", label,
                    format_bytes_detail(evt.tool_call_argument_bytes),
                    tool_name, evt.tool_call.id, evt.tool_index, false);
            }
            break;
        case StreamEventType::ToolCall:
            if (!first_output_recorded) {
                first_output_recorded = true;
                if (session_manager_) {
                    session_manager_->record_trajectory_event(
                        "model_first_output",
                        {{"step_index", model_step_index},
                         {"attempt", provider_attempt},
                         {"channel", "tool_call"}});
                }
            }
            {
                ToolCall native_call = evt.tool_call;
                native_call.function_name =
                    tools_.resolve_model_tool_name_to_native(
                        native_call.function_name);
                {
                    std::lock_guard<std::mutex> lk(resp_mu);
                    result.accumulated.tool_calls.push_back(std::move(native_call));
                }
            }
            break;
        case StreamEventType::Done: {
            // 透传服务端上报的 finish_reason(可能为空 — 部分兼容网关不发)。
            // 非空才覆盖,保持 "stop" 兜底默认值。
            std::lock_guard<std::mutex> lk(resp_mu);
            if (!evt.finish_reason.empty()) {
                result.accumulated.finish_reason = evt.finish_reason;
            }
            if (evt.content_parts.is_array() && !evt.content_parts.empty()) {
                result.accumulated.content_parts = evt.content_parts;
            }
            // 文本形式工具调用的诊断(fix-feedback-0924 第 3 条):主循环据此
            // 决定是否注入纠正提示重试,而不是把被拒的回复当纯文本静默结束。
            result.accumulated.text_tool_calls = evt.text_tool_calls;
            break;
        }
        case StreamEventType::Usage: {
            TokenUsage usage = evt.usage;
            usage.context_breakdown = reconcile_context_usage_breakdown(
                bundle.context_usage_estimate,
                usage.prompt_tokens);
            {
                std::lock_guard<std::mutex> lk(resp_mu);
                result.accumulated.usage = usage;
            }
            // Usage is provisional until this provider attempt completes. A
            // later Retry event clears it together with text/reasoning/tools;
            // publishing here would double-count the failed attempt.
            break;
        }
        case StreamEventType::Retry:
            result.provider_error_info = evt.provider_error;
            // A Retry event always precedes a full replay of the immutable
            // provider request. Clear every provisional response component so
            // partial output from the failed attempt cannot be duplicated.
            {
                std::lock_guard<std::mutex> lk(resp_mu);
                result.accumulated = ChatResponse{};
                result.accumulated.finish_reason = "stop";
            }
            reasoning_bytes = 0;
            reasoning_fragments = 0;
            ++provider_attempt;
            first_output_recorded = false;
            reasoning_title_found = false;
            step_text_started = false;
            reset_activity_for_step();
            text_preamble_scanner_.reset();
            if (callbacks_.on_stream_retry_reset) {
                callbacks_.on_stream_retry_reset();
            }
            {
                CompactResult reset_result;
                std::vector<ChatMessage> visible_reset_messages =
                    session_manager_
                        ? session_manager_->load_active_messages()
                        : history_->view();
                events_.emit(
                    SessionEventKind::TranscriptReplace,
                    build_transcript_replace_payload(
                        visible_reset_messages, reset_result));
            }
            emit_retry_lifecycle(
                evt.provider_error, true, false);
            break;
        case StreamEventType::RetryResume:
            emit_retry_lifecycle(
                evt.provider_error, false, false);
            break;
        case StreamEventType::Error:
            if ((evt.provider_error.kind == ProviderErrorKind::UserCancelled ||
                 evt.error == "Request cancelled") &&
                abort_signal_.raw().load()) {
                break;
            }
            result.provider_error_seen = true;
            result.provider_error_info = evt.provider_error;
            if (!result.provider_error_info.has_error()) {
                result.provider_error_info.kind = ProviderErrorKind::Unknown;
                result.provider_error_info.display_message = evt.error;
            }
            if (result.provider_error_info.display_message.empty()) {
                result.provider_error_info.display_message = evt.error;
            }
            break;
        }
    };

    LOG_INFO("Calling chat_stream with " + std::to_string(bundle.messages_with_system.size()) + " messages");
    try {
        emit_progress(
            "model_waiting", "正在等待模型响应",
            std::string{}, std::string{}, std::string{}, -1, true);
        agent::ActiveProviderScope active_provider(*active_provider_slot_, provider);
        provider->chat_stream(bundle.messages_with_system, bundle.tool_defs,
                              stream_callback, &abort_signal_.flag_for_legacy_api());
        active_provider.reset();
        // 流结束:把扫描器扣住的字节结清(半截开标签按正文放行,没闭合的标签
        // 正文仍算前言)。
        publish_scanned(text_preamble_scanner_.flush());
        LOG_INFO("chat_stream returned. content_len=" +
                 std::to_string(result.accumulated.content.size()) +
                 " tool_calls=" + std::to_string(result.accumulated.tool_calls.size()));
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("chat_stream exception: ") + e.what());
        result.provider_error_seen = true;
        result.provider_error_info.kind = ProviderErrorKind::Unknown;
        result.provider_error_info.display_message = e.what();
    } catch (...) {
        LOG_ERROR("chat_stream threw an unknown exception");
        result.provider_error_seen = true;
        result.provider_error_info.kind = ProviderErrorKind::Unknown;
        result.provider_error_info.display_message =
            "Provider request failed with an unknown exception";
    }

    TokenUsage final_usage;
    {
        std::lock_guard<std::mutex> lk(resp_mu);
        final_usage = result.accumulated.usage;
    }
    result.provider_attempt = provider_attempt;
    if (!result.provider_error_seen &&
        !abort_signal_.raw().load() &&
        final_usage.has_data) {
        // Record at the same boundary as live accounting, before consumers
        // can throw and transfer control to worker recovery.
        accumulate_turn_usage(
            active_turn_usage_, active_turn_usage_initialized_, final_usage);
        last_api_total_tokens_.store(
            final_usage.total_tokens > 0
                ? final_usage.total_tokens
                : final_usage.prompt_tokens,
            std::memory_order_relaxed);
        account_goal_usage(final_usage.total_tokens, false);
        if (callbacks_.on_usage) {
            callbacks_.on_usage(final_usage);
        }
        if (session_manager_) {
            session_manager_->record_token_usage(final_usage);
        }
        events_.emit(
            SessionEventKind::Usage,
            model_step_usage_to_json(final_usage));
    }

    return result;
}

} // namespace acecode
