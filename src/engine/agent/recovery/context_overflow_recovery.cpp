#include "context_overflow_recovery.hpp"
#include "pa_rescue_host.hpp"
#include "provider_error_report.hpp"
#include "agent/agent_callbacks.hpp"
#include "agent/compaction/compaction_controller.hpp"
#include "agent/compaction/compact.hpp"
#include "agent/goal/goal_runtime.hpp"
#include "agent/model_step/active_model_view.hpp"
#include "agent/request/provider_history.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "pa/pa_quirks.hpp"
#include "session/event_dispatcher.hpp"
#include "session/token_tracker.hpp"
#include "utils/abort_signal.hpp"
#include "utils/logger.hpp"
#include <algorithm>
#include <utility>

namespace acecode::agent {
using detail::recovered_provider_messages;
using detail::provider_error_to_json;
using detail::provider_error_summary_for_log;

RecoveryDecision ContextOverflowRecovery::resolve(
    const ProviderCallResult& result, const std::vector<ChatMessage>& messages_with_system,
    RequestRecoveryState& state, int declared_window, SessionManager* session) {
    std::optional<std::string> turn_timing_status;
    auto& recovery_stage = state.stage;
    auto& emergency_request_profile = state.emergency_profile;
    const ActiveModelView model(result.provider_snapshot, declared_window);
    if (!result.provider_error_seen) {
        model.note_accepted(messages_with_system);
        // 服务端收下了这次请求:PA 兜底的这一轮到此结束,后面再被拒是新一轮。
        state.pa_episode = {};
        return {HandleErrorResult::Proceed, turn_timing_status};
    }

    if (abort_.raw()) {
        return {HandleErrorResult::Break, turn_timing_status};
    }

    const bool model_output_seen =
        !result.accumulated.content.empty() ||
        !result.accumulated.reasoning_content.empty() ||
        result.accumulated.has_tool_calls();
    const int request_tokens = estimate_message_tokens(messages_with_system);
    const int context_window = declared_window;
    const bool context_overflow =
        is_context_overflow_error(result.provider_error_info);
    LOG_WARN("Provider error before turn completion; " +
             provider_error_summary_for_log(result.provider_error_info) +
             " request_estimated_tokens=" +
             std::to_string(request_tokens) +
             " context_window=" + std::to_string(context_window) +
             " messages_with_system=" +
             std::to_string(messages_with_system.size()) +
             " model_output_seen=" +
             (model_output_seen ? "true" : "false") +
             " context_overflow=" +
             (context_overflow ? "true" : "false"));

    bool pa_rescue_exhausted = false;
    if (context_overflow && !model_output_seen &&
        pa::is_context_overflow(result.provider_error_info)) {
        // PA 特征报文走专用兜底(src/pa/pa_overflow_rescue):不设修复次数
        // 上限,缩到底还被拒就等。下面的通用三级恢复链只服务其它 provider。
        PaRescueAdapter host(history_, transcript_, compaction_, retry_, callbacks_,
                             events_, abort_, session, model);
        if (pa::run_rescue(host, state.pa_episode, result.provider_error_info,
                           request_tokens, emergency_request_profile)) {
            state.skip_auto_compact_once = true;
            return {HandleErrorResult::Continue, turn_timing_status};
        }
        if (abort_.raw()) return {HandleErrorResult::Break, turn_timing_status};
        pa_rescue_exhausted = true;
    } else if (context_overflow && !model_output_seen) {
        // 先记账再恢复:这一轮已经撞墙了救不回来,但下一轮可以不撞。
        if (const auto notice = model.note_rejected(request_tokens)) {
            transcript_.emit_transcript_system_message(session, notice->text, notice->metadata);
        }
        if (recovery_stage == ContextRecoveryStage::Normal) {
            const int history_tokens = estimate_message_tokens(
                recovered_provider_messages(
                    history_.view(), "context-overflow-estimate"));
            const int fixed_tokens = (std::max)(0, request_tokens - history_tokens);
            int target_total = (std::max)(1, request_tokens * 2 / 3);
            if (context_window > 0) {
                target_total = (std::min)(
                    target_total, context_window * 7 / 10);
            }
            ThreadRepairOptions options;
            options.trigger = "repair-context-overflow";
            options.target_tokens = (std::max)(
                1, target_total - fixed_tokens);
            options.force_prune_one_group = true;
            auto repair = history_.repair(session, options);
            LOG_WARN("[thread-repair] automatic status=" +
                     std::string(to_string(repair.status)) +
                     " pre_tokens=" + std::to_string(repair.pre_tokens) +
                     " post_tokens=" + std::to_string(repair.post_tokens) +
                     " pruned_groups=" +
                     std::to_string(repair.pruned_groups) +
                     " reason=" + repair.reason);
            if (repair.repaired()) {
                recovery_stage = ContextRecoveryStage::HistoryRepaired;
                compaction_.mark_history_repaired();
                if (callbacks_.on_stream_retry_reset) {
                    callbacks_.on_stream_retry_reset();
                }
                events_.emit(SessionEventKind::AgentProgress, nlohmann::json{
                    {"phase", "context_repair"},
                    {"label", "Retrying with repaired thread history"},
                    {"detail", repair.reason},
                });
                return {HandleErrorResult::Continue, turn_timing_status};
            }
            recovery_stage = ContextRecoveryStage::EmergencyProfile;
            emergency_request_profile = true;
            if (callbacks_.on_stream_retry_reset) {
                callbacks_.on_stream_retry_reset();
            }
            LOG_WARN("[thread-repair] history exhausted; retrying once with "
                     "the emergency request profile");
            return {HandleErrorResult::Continue, turn_timing_status};
        }
        if (recovery_stage == ContextRecoveryStage::HistoryRepaired) {
            recovery_stage = ContextRecoveryStage::EmergencyProfile;
            emergency_request_profile = true;
            if (callbacks_.on_stream_retry_reset) {
                callbacks_.on_stream_retry_reset();
            }
            LOG_WARN("[thread-repair] repaired history was still rejected; "
                     "retrying once with the emergency request profile");
            return {HandleErrorResult::Continue, turn_timing_status};
        }
        LOG_WARN("[thread-repair] emergency request profile was still rejected; "
                 "automatic recovery is exhausted");
    } else if (context_overflow && model_output_seen) {
        LOG_WARN("[thread-repair] context overflow arrived after model output; "
                 "not replaying the model step or tool activity");
    }

    nlohmann::json metadata;
    metadata["provider_error"] = provider_error_to_json(result.provider_error_info);
    if (context_overflow) {
        metadata["thread_repair_exhausted"] =
            pa_rescue_exhausted ||
            recovery_stage == ContextRecoveryStage::EmergencyProfile;
        metadata["partial_model_output"] = model_output_seen;
    }
    if (pa_rescue_exhausted) metadata["pa_rescue_exhausted"] = true;
    turn_timing_status = "error";
    std::string display_message = result.provider_error_info.display_message;
    if (pa_rescue_exhausted) {
        display_message +=
            " 服务端在 " + std::to_string(pa::PA_RESCUE_MAX_WAIT_RETRIES) +
            " 次等待重试后仍拒收已缩到最小的请求，本回合放弃；稍后重新发送即可"
            "继续。";
    } else if (context_overflow &&
               recovery_stage == ContextRecoveryStage::EmergencyProfile &&
               !model_output_seen) {
        display_message +=
            " Automatic thread repair and the emergency request profile were "
            "both exhausted; the fixed context or current input may exceed the "
            "provider's actual limit.";
    }
    transcript_.dispatch_message("error", "[Error] " + display_message, false,
                                  std::move(metadata), nlohmann::json::array());
    LOG_WARN("Provider stream failed; ending turn without assistant message: " +
             log_truncate(result.provider_error_info.display_message, 500));
    goal_.stop_after_error(session, result.provider_error_info);
    return {HandleErrorResult::Break, turn_timing_status};
}

} // namespace acecode::agent
