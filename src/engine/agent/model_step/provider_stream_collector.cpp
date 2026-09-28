#include "provider_stream_collector.hpp"
#include "active_provider_slot.hpp"
#include "model_step_recorder.hpp"
#include "turn_usage_accountant.hpp"
#include "agent/agent_callbacks.hpp"
#include "agent/compaction/compact.hpp"
#include "agent/progress/activity_narrator.hpp"
#include "agent/progress/retry_progress.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_queries.hpp"
#include "llm/text_preamble_tags.hpp"
#include "session/event_dispatcher.hpp"
#include "session/session_manager.hpp"
#include "tool/tool_executor.hpp"
#include "utils/abort_signal.hpp"
#include "utils/lifetime_token.hpp"
#include "utils/logger.hpp"
#include <mutex>
#include <utility>

namespace acecode::agent {

using detail::human_bytes;
using detail::format_bytes_detail;
using detail::build_transcript_replace_payload;

// Scoped fixed dependencies. The provider callback carries only LifetimeRef:
// revocation waits for admitted calls and suppresses callbacks retained by a
// provider after chat_stream returns. No reference into this call can escape.
struct ProviderStreamCollector::Call {
    Call(ProviderStreamCollector& owner, SessionManager* session,
         ApiRequestBundle request, ProgressEmitter progress, int step,
         std::shared_ptr<LlmProvider> provider)
        : owner_(owner), session_manager_(session), bundle(std::move(request)),
          emit_progress(std::move(progress)), model_step_index(step),
          callbacks_(owner.callbacks_), concrete(owner.activity_.enabled()) {
        result.accumulated.finish_reason = "stop";
        result.provider_snapshot = std::move(provider);
        owner_.activity_.reset_step();
        text_preamble_scanner_.reset();
    }

    void publish_visible_text(const std::string& text) {
        if (text.empty()) return;
        if (concrete && !step_text_started &&
            text.find_first_not_of(" \t\r\n") != std::string::npos) {
            step_text_started = true;
            emit_progress("responding", agent::ActivityNarrator::responding_label(),
                          std::string{}, std::string{}, std::string{}, -1, true);
        }
        if (callbacks_.on_delta) {
            callbacks_.on_delta(text);
        }
        owner_.events_.emit(SessionEventKind::Token, nlohmann::json{{"text", text}});
    }
    void publish_scanned(llm::TextPreambleScanner::Output out) {
        publish_visible_text(out.visible);
    }
    void on_event(const StreamEvent& evt) {
        switch (evt.type) {
        case StreamEventType::Delta:
            if (!evt.content.empty() && !first_output_recorded) {
                first_output_recorded = true;
                owner_.recorder_.first_output(session_manager_, model_step_index,
                                               provider_attempt, "content");
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
                owner_.recorder_.first_output(session_manager_, model_step_index,
                                               provider_attempt, "reasoning");
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
                const auto found = agent::ActivityNarrator::reasoning_title(reasoning_so_far);
                if (!found.title.empty()) {
                    reasoning_title_found = true;
                    owner_.activity_.publish_phase(found, emit_progress);
                }
            }
            // 开启具体进度提示时,发射口会把这条换成本步标题或场景文案。
            emit_progress("reasoning", "正在推理",
                "片段 " + std::to_string(reasoning_fragments) + ", " +
                human_bytes(reasoning_bytes),
                std::string{}, std::string{}, -1, false);
            owner_.events_.emit(SessionEventKind::Reasoning, nlohmann::json{{"text", evt.content}});
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
                owner_.recorder_.first_output(session_manager_, model_step_index,
                                               provider_attempt, "tool_call");
            }
            {
                const std::string tool_name =
                    owner_.tools_.resolve_model_tool_name_to_native(
                        evt.tool_call.function_name);
                const std::string label = tool_name.empty()
                    ? "正在准备工具调用"
                    : "正在准备调用 " + tool_name;
                // 具体进度提示:记下本步流出来的工具,发射口据此拼「正在读取 2 个文件」。
                owner_.activity_.note_planned_tool(evt.tool_index, tool_name);
                emit_progress("tool_planning", label,
                    format_bytes_detail(evt.tool_call_argument_bytes),
                    tool_name, evt.tool_call.id, evt.tool_index, false);
            }
            break;
        case StreamEventType::ToolCall:
            if (!first_output_recorded) {
                first_output_recorded = true;
                owner_.recorder_.first_output(session_manager_, model_step_index,
                                               provider_attempt, "tool_call");
            }
            {
                ToolCall native_call = evt.tool_call;
                native_call.function_name =
                    owner_.tools_.resolve_model_tool_name_to_native(
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
            owner_.activity_.reset_step();
            text_preamble_scanner_.reset();
            if (callbacks_.on_stream_retry_reset) {
                callbacks_.on_stream_retry_reset();
            }
            {
                CompactResult reset_result;
                std::vector<ChatMessage> visible_reset_messages =
                    session_manager_
                        ? session_manager_->load_active_messages()
                        : owner_.history_.view();
                owner_.events_.emit(
                    SessionEventKind::TranscriptReplace,
                    build_transcript_replace_payload(
                        visible_reset_messages, reset_result));
            }
            owner_.retry_.standard(
                evt.provider_error, true, false);
            break;
        case StreamEventType::RetryResume:
            owner_.retry_.standard(
                evt.provider_error, false, false);
            break;
        case StreamEventType::Error:
            if ((evt.provider_error.kind == ProviderErrorKind::UserCancelled ||
                 evt.error == "Request cancelled") &&
                owner_.abort_.raw().load()) {
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
    }

    ProviderStreamCollector& owner_;
    SessionManager* const session_manager_; // nullable, borrowed for this call
    ApiRequestBundle bundle;
    ProgressEmitter emit_progress;
    int model_step_index;
    AgentCallbacks callbacks_;
    bool concrete;
    ProviderCallResult result;
    std::mutex resp_mu;
    std::size_t reasoning_bytes = 0;
    int reasoning_fragments = 0;
    int provider_attempt = 1;
    bool first_output_recorded = false;
    bool reasoning_title_found = false;
    bool step_text_started = false;
    llm::TextPreambleScanner text_preamble_scanner_;
    LifetimeToken lifetime; // first destroyed; all callback state outlives revoke
};

ProviderCallResult ProviderStreamCollector::collect(
    const std::shared_ptr<LlmProvider>& provider, const ApiRequestBundle& bundle,
    const ProgressEmitter& emit_progress, int model_step_index,
    TurnUsageRecord& record, SessionManager* session) {
    Call call(*this, session, bundle, emit_progress, model_step_index, provider);
    auto& result = call.result;
    auto& resp_mu = call.resp_mu;
    auto& provider_attempt = call.provider_attempt;
    auto stream_callback = [ref = call.lifetime.ref(call)](const StreamEvent& event) {
        ref.with([&event](Call& state) { state.on_event(event); });
    };
    LOG_INFO("Calling chat_stream with " + std::to_string(bundle.messages_with_system.size()) + " messages");
    try {
        emit_progress(
            "model_waiting", "正在等待模型响应",
            std::string{}, std::string{}, std::string{}, -1, true);
        agent::ActiveProviderScope active_provider(active_provider_, provider);
        provider->chat_stream(bundle.messages_with_system, bundle.tool_defs,
                              stream_callback, &abort_.flag_for_legacy_api());
        active_provider.reset();
        call.lifetime.revoke();
        // 流结束:把扫描器扣住的字节结清(半截开标签按正文放行,没闭合的标签
        // 正文仍算前言)。
        call.publish_scanned(call.text_preamble_scanner_.flush());
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

    call.lifetime.revoke();
    TokenUsage final_usage;
    {
        std::lock_guard<std::mutex> lk(resp_mu);
        final_usage = result.accumulated.usage;
    }
    result.provider_attempt = provider_attempt;
    if (!result.provider_error_seen &&
        !abort_.raw().load() &&
        final_usage.has_data) {
        usage_.accept(record, final_usage, session);
    }

    return result;
}
} // namespace acecode::agent
