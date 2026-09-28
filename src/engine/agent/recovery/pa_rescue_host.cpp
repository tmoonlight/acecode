#include "agent/agent_loop.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/request/provider_history.hpp"
#include "pa/pa_overflow_rescue.hpp"
#include "permissions/interaction_mode.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/system_notice.hpp"
#include "session/thread_goal_store.hpp"
#include "session/thread_repair.hpp"
#include "utils/logger.hpp"
#include "utils/time.hpp"
#include "utils/uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode {

using agent::detail::recovered_provider_messages;
using utils::now_epoch_ms;

bool AgentLoop::wait_for_pa_rescue_delay(int wait_ms) {
    const int scaled = pa::scaled_rescue_wait_ms(wait_ms);
    return !abort_signal_.wait_for(std::chrono::milliseconds(scaled));
}

void AgentLoop::emit_pa_rescue_wait_progress(const ProviderErrorInfo& error,
                                             const pa::RescuePlan& plan,
                                             int attempt,
                                             int max_attempts,
                                             bool waiting) {
    // 复用 provider 层重试的展示通道:TUI 走 on_model_retry 的等待短语,Web 走
    // model_retry 进度事件的倒计时。文案换成兜底自己的,别让用户以为是断网。
    ProviderErrorInfo info = error;
    info.retry_attempt = attempt;
    info.retry_max_attempts = max_attempts;
    info.retry_delay_ms = waiting ? pa::scaled_rescue_wait_ms(plan.wait_ms) : 0;
    if (waiting) {
        if (callbacks_.on_model_retry) callbacks_.on_model_retry(info);
    } else if (callbacks_.on_model_retry_resume) {
        callbacks_.on_model_retry_resume();
    }

    const std::int64_t now_ms = now_epoch_ms();
    nlohmann::json payload{
        {"phase", waiting ? "model_retry" : "model_waiting"},
        {"label", waiting ? plan.label : std::string("正在重新发送请求")},
        {"detail",
         waiting ? std::string("服务端报「请求上下文过大」，按 PA 兜底策略等待后重发")
                 : std::string{}},
        {"started_at_ms", now_ms},
        {"retry_attempt", attempt},
        {"retry_delay_ms", info.retry_delay_ms},
        {"retry_at_ms", now_ms + info.retry_delay_ms},
        {"retry_max_attempts", max_attempts},
    };
    EventDispatcher::EmitOptions opts;
    opts.buffered = true;
    opts.coalesce_key = "agent_progress";
    events_.emit(SessionEventKind::AgentProgress, std::move(payload), opts);
}

AgentLoop::HandleErrorResult AgentLoop::run_pa_overflow_rescue(
    const ProviderErrorInfo& error,
    int request_tokens,
    bool& emergency_request_profile) {
    pa::RescueState& state = pa_rescue_state_;
    if (!state.active) state = pa::RescueState{};
    const int history_tokens = estimate_message_tokens(
        recovered_provider_messages(history_->view(), "pa-rescue-estimate"));

    // 一次调用可能连走几步:收缩腾不出空间时不重发,立刻换下一招。
    for (;;) {
        pa::RescueInputs inputs;
        inputs.request_tokens = request_tokens;
        inputs.history_tokens = history_tokens;
        inputs.emergency_profile = emergency_request_profile;
        const pa::RescuePlan plan = pa::next_rescue_step(state, inputs);
        pa::advance_rescue_state(state, plan);
        LOG_WARN("[pa-rescue] action=" + std::string(pa::to_string(plan.action)) +
                 " request_estimated_tokens=" + std::to_string(request_tokens) +
                 " history_estimated_tokens=" + std::to_string(history_tokens) +
                 " same_request_retries=" +
                 std::to_string(state.same_request_retries) +
                 " shrink_rounds=" + std::to_string(state.shrink_rounds) +
                 " wait_retries=" + std::to_string(state.wait_retries) +
                 " emergency_profile=" +
                 (emergency_request_profile ? "true" : "false") +
                 " target_history_tokens=" +
                 std::to_string(plan.target_history_tokens) +
                 " wait_ms=" + std::to_string(plan.wait_ms) +
                 " label=" + plan.label);
        if (plan.record_rejection) note_pa_context_rejection(request_tokens);

        switch (plan.action) {
            case pa::RescueAction::RetrySameRequest:
            case pa::RescueAction::WaitAndRetry: {
                const bool waiting_for_recovery =
                    plan.action == pa::RescueAction::WaitAndRetry;
                const int attempt = waiting_for_recovery
                    ? state.wait_retries : state.same_request_retries;
                const int max_attempts = waiting_for_recovery
                    ? pa::PA_RESCUE_MAX_WAIT_RETRIES
                    : pa::PA_RESCUE_SAME_REQUEST_RETRIES;
                if (!waiting_for_recovery && state.same_request_retries == 1) {
                    emit_transcript_system_message(
                        "[智能压缩] 服务端报「请求上下文过大」，先原样重发确认"
                        "是否为瞬时故障；确认拒收后才会收缩历史。",
                        make_system_notice_metadata("context_retrying"));
                } else if (waiting_for_recovery && state.wait_retries == 1) {
                    emit_transcript_system_message(
                        "[智能压缩] 请求已缩到最小仍被服务端拒收；将按 5 秒起、"
                        "最长 60 秒的间隔反复重试（最多 " +
                        std::to_string(pa::PA_RESCUE_MAX_WAIT_RETRIES) +
                        " 次），可随时停止。", make_system_notice_metadata("context_waiting",
                            {{"attempts", pa::PA_RESCUE_MAX_WAIT_RETRIES}}));
                }
                emit_pa_rescue_wait_progress(
                    error, plan, attempt, max_attempts, true);
                if (!wait_for_pa_rescue_delay(plan.wait_ms)) {
                    return HandleErrorResult::Break;
                }
                emit_pa_rescue_wait_progress(
                    error, plan, attempt, max_attempts, false);
                if (callbacks_.on_stream_retry_reset) {
                    callbacks_.on_stream_retry_reset();
                }
                skip_auto_compact_once_ = true;
                return HandleErrorResult::Continue;
            }
            case pa::RescueAction::ShrinkHistory: {
                ThreadRepairOptions options;
                options.trigger = "repair-pa-overflow";
                options.target_tokens = plan.target_history_tokens;
                options.force_prune_one_group = true;
                options.clear_tool_outputs = true;
                options.keep_recent_tool_outputs = 1;
                auto repair = history_->repair(session_manager_, options);
                LOG_WARN("[pa-rescue] shrink status=" +
                         std::string(to_string(repair.status)) +
                         " pre_tokens=" + std::to_string(repair.pre_tokens) +
                         " post_tokens=" + std::to_string(repair.post_tokens) +
                         " pruned_groups=" +
                         std::to_string(repair.pruned_groups) +
                         " cleared_tool_outputs=" +
                         std::to_string(repair.cleared_tool_outputs) +
                         " reason=" + repair.reason);
                if (!repair.repaired()) {
                    // 一点空间都没腾出来:这一轮不再提议收缩,立刻换下一招。
                    state.shrink_exhausted = true;
                    continue;
                }
                compact_generation_.fetch_add(1, std::memory_order_relaxed);
                last_api_total_tokens_.store(0, std::memory_order_relaxed);
                if (callbacks_.on_stream_retry_reset) {
                    callbacks_.on_stream_retry_reset();
                }
                events_.emit(SessionEventKind::AgentProgress, nlohmann::json{
                    {"phase", "context_repair"},
                    {"label", plan.label},
                    {"detail", repair.reason},
                });
                emit_transcript_system_message(
                    "[智能压缩] 服务端拒收请求（第 " +
                    std::to_string(state.shrink_rounds) +
                    " 次收缩）：已丢弃最旧的 " +
                    std::to_string(repair.pruned_groups) + " 组历史、清除 " +
                    std::to_string(repair.cleared_tool_outputs) +
                    " 条旧工具输出后重试。", make_system_notice_metadata("context_history_pruned",
                        {{"round", state.shrink_rounds}, {"groups", repair.pruned_groups},
                         {"outputs", repair.cleared_tool_outputs}}));
                skip_auto_compact_once_ = true;
                return HandleErrorResult::Continue;
            }
            case pa::RescueAction::EmergencyProfile: {
                emergency_request_profile = true;
                if (callbacks_.on_stream_retry_reset) {
                    callbacks_.on_stream_retry_reset();
                }
                events_.emit(SessionEventKind::AgentProgress, nlohmann::json{
                    {"phase", "context_repair"},
                    {"label", plan.label},
                    {"detail", "去掉工具定义与注入上下文，仅保留核心工具"},
                });
                emit_transcript_system_message("[智能压缩] " + plan.label + "。",
                    make_system_notice_metadata("context_emergency"));
                skip_auto_compact_once_ = true;
                return HandleErrorResult::Continue;
            }
            case pa::RescueAction::GiveUp:
                LOG_WARN("[pa-rescue] giving up: " + plan.label);
                return HandleErrorResult::Break;
        }
    }
}

} // namespace acecode
