#include "agent/agent_loop.hpp"
#include "agent/compaction/compact.hpp"
#include "agent/recovery/provider_error_report.hpp"
#include "agent/request/provider_history.hpp"
#include "llm/tool_protocol_names.hpp"
#include "pa/pa_overflow_rescue.hpp"
#include "pa/pa_quirks.hpp"
#include "permissions/shell_write_guard.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/thread_goal_store.hpp"
#include "session/thread_repair.hpp"
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
#include <thread>

namespace acecode {

using agent::detail::recovered_provider_messages;
using agent::detail::provider_error_to_json;
using agent::detail::provider_error_summary_for_log;

AgentLoop::HandleErrorResult AgentLoop::handle_provider_error(
    ProviderCallResult& result,
    const std::vector<ChatMessage>& messages_with_system,
    std::string& turn_timing_status,
    ContextRecoveryStage& recovery_stage,
    bool& emergency_request_profile) {
    if (!result.provider_error_seen) {
        note_pa_context_accepted(messages_with_system);
        // 服务端收下了这次请求:PA 兜底的这一轮到此结束,后面再被拒是新一轮。
        pa_rescue_state_ = pa::RescueState{};
        return HandleErrorResult::Proceed;
    }

    if (abort_requested_) {
        return HandleErrorResult::Break;
    }

    const bool model_output_seen =
        !result.accumulated.content.empty() ||
        !result.accumulated.reasoning_content.empty() ||
        result.accumulated.has_tool_calls();
    const int request_tokens = estimate_message_tokens(messages_with_system);
    const int context_window = context_window_.load(std::memory_order_relaxed);
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
        const HandleErrorResult rescue = run_pa_overflow_rescue(
            result.provider_error_info, request_tokens,
            emergency_request_profile);
        if (rescue == HandleErrorResult::Continue) return rescue;
        if (abort_requested_) return HandleErrorResult::Break;
        pa_rescue_exhausted = true;
    } else if (context_overflow && !model_output_seen) {
        // 先记账再恢复:这一轮已经撞墙了救不回来,但下一轮可以不撞。
        note_pa_context_rejection(request_tokens);
        if (recovery_stage == ContextRecoveryStage::Normal) {
            const int history_tokens = estimate_message_tokens(
                recovered_provider_messages(
                    messages_, "context-overflow-estimate"));
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
            auto repair = apply_thread_repair(
                session_manager_, messages_, options);
            LOG_WARN("[thread-repair] automatic status=" +
                     std::string(to_string(repair.status)) +
                     " pre_tokens=" + std::to_string(repair.pre_tokens) +
                     " post_tokens=" + std::to_string(repair.post_tokens) +
                     " pruned_groups=" +
                     std::to_string(repair.pruned_groups) +
                     " reason=" + repair.reason);
            if (repair.repaired()) {
                recovery_stage = ContextRecoveryStage::HistoryRepaired;
                compact_generation_.fetch_add(1, std::memory_order_relaxed);
                last_api_total_tokens_.store(0, std::memory_order_relaxed);
                if (callbacks_.on_stream_retry_reset) {
                    callbacks_.on_stream_retry_reset();
                }
                events_.emit(SessionEventKind::AgentProgress, nlohmann::json{
                    {"phase", "context_repair"},
                    {"label", "Retrying with repaired thread history"},
                    {"detail", repair.reason},
                });
                return HandleErrorResult::Continue;
            }
            recovery_stage = ContextRecoveryStage::EmergencyProfile;
            emergency_request_profile = true;
            if (callbacks_.on_stream_retry_reset) {
                callbacks_.on_stream_retry_reset();
            }
            LOG_WARN("[thread-repair] history exhausted; retrying once with "
                     "the emergency request profile");
            return HandleErrorResult::Continue;
        }
        if (recovery_stage == ContextRecoveryStage::HistoryRepaired) {
            recovery_stage = ContextRecoveryStage::EmergencyProfile;
            emergency_request_profile = true;
            if (callbacks_.on_stream_retry_reset) {
                callbacks_.on_stream_retry_reset();
            }
            LOG_WARN("[thread-repair] repaired history was still rejected; "
                     "retrying once with the emergency request profile");
            return HandleErrorResult::Continue;
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
    dispatch_message("error", "[Error] " + display_message, false,
                     std::move(metadata));
    LOG_WARN("Provider stream failed; ending turn without assistant message: " +
             log_truncate(result.provider_error_info.display_message, 500));
    stop_active_goal_after_turn_error(result.provider_error_info);
    return HandleErrorResult::Break;
}

} // namespace acecode
