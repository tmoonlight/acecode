#include "compaction_controller.hpp"
#include "compact.hpp"
#include "agent/callbacks_slot.hpp"
#include "agent/boundary/workspace_boundary.hpp"
#include "agent/hook_bridge/agent_hook_bridge.hpp"
#include "agent/model_step/active_model_view.hpp"
#include "agent/model_step/active_provider_slot.hpp"
#include "agent/progress/retry_progress.hpp"
#include "agent/request/api_request_builder.hpp"
#include "agent/request/provider_history.hpp"
#include "agent/transcript/conversation_history.hpp"
#include "agent/transcript/transcript_writer.hpp"
#include "agent/transcript/trajectory_recorder.hpp"
#include "agent/turn/busy_cycle.hpp"
#include "session/compact_checkpoint.hpp"
#include "session/compact_notice.hpp"
#include "session/event_dispatcher.hpp"
#include "session/session_manager.hpp"
#include "session/system_notice.hpp"
#include "session/task_suggestion_store.hpp"
#include "session/thread_repair.hpp"
#include "session/token_tracker.hpp"
#include "tool/mtime_tracker.hpp"
#include "utils/abort_signal.hpp"
#include "utils/logger.hpp"
#include "utils/encoding.hpp"
#include "utils/time.hpp"
#include "utils/uuid.hpp"
#include <algorithm>
#include <limits>
#include <utility>

namespace acecode::agent {
using detail::recovered_provider_messages;
using utils::now_epoch_ms;

bool CompactionController::mechanical_fallback(
    const CompactionInputs& inputs,
    int request_tokens,
    int context_window,
    const std::string& compact_notice_id,
    const std::string& summarization_error) {
    // 目标规模与上下文溢出恢复路径同口径:降到窗口的 70%,给下一轮留出余量。
    const int history_tokens = estimate_message_tokens(
        recovered_provider_messages(history_.view(), "compact-fallback-estimate"));
    const int fixed_tokens = (std::max)(0, request_tokens - history_tokens);
    int target_total = (std::max)(1, request_tokens * 2 / 3);
    if (context_window > 0) {
        target_total = (std::min)(target_total, context_window * 7 / 10);
    }

    ThreadRepairOptions options;
    options.trigger = "repair-compact-fallback";
    options.target_tokens = (std::max)(1, target_total - fixed_tokens);
    // 强制至少丢掉一组:摘要已经失败了,原地不动地"成功"只会让调用方以为
    // 腾出了空间,下一轮继续撞同一堵墙。
    options.force_prune_one_group = true;
    // 只剩当前回合时整组丢不掉,但本回合里堆着的旧工具输出还能清 —— 单回合
    // 读了一堆大文件正是摘要请求本身也会被拒的那种场景。
    options.clear_tool_outputs = true;
    options.keep_recent_tool_outputs = 1;

    auto repair = history_.repair(inputs.session, options);
    LOG_WARN("[compact-fallback] mechanical prune after summarization failure; "
             "status=" + std::string(to_string(repair.status)) +
             " pre_tokens=" + std::to_string(repair.pre_tokens) +
             " post_tokens=" + std::to_string(repair.post_tokens) +
             " pruned_groups=" + std::to_string(repair.pruned_groups) +
             " cleared_tool_outputs=" +
             std::to_string(repair.cleared_tool_outputs) +
             " target_tokens=" + std::to_string(options.target_tokens) +
             " reason=" + repair.reason);
    if (!repair.repaired()) {
        return false;
    }

    compact_generation_.fetch_add(1, std::memory_order_relaxed);
    last_api_total_tokens_.store(0, std::memory_order_relaxed);

    events_.emit(SessionEventKind::AgentProgress, nlohmann::json{
        {"phase", "context_repair"},
        {"label", "Compaction failed; pruned oldest history instead"},
        {"detail", repair.reason},
    });
    transcript_.emit_transcript_system_message(inputs.session,
        "[智能压缩] 摘要压缩失败(" + log_truncate(summarization_error, 160) +
        "),已改为丢弃最旧的 " + std::to_string(repair.pruned_groups) +
        " 组历史、清除 " + std::to_string(repair.cleared_tool_outputs) +
        " 条旧工具输出腾出空间,会话继续。",
        make_compact_notice_metadata(compact_notice_id, "warning", false,
            {{"error", summarization_error}, {"groups", repair.pruned_groups},
             {"outputs", repair.cleared_tool_outputs}}));
    return true;
}

bool CompactionController::run_auto(const CompactionInputs& inputs) {
    const int context_window = ActiveModelView(inputs.provider, inputs.request.context_window, environment_).effective_window();
    auto initial_context = requests_.initial_context(inputs.request);
    const auto active_history =
        recovered_provider_messages(history_.view(), "auto-compact");
    auto estimated_request = initial_context;
    estimated_request.insert(
        estimated_request.end(), active_history.begin(), active_history.end());
    const int pre_tokens = estimate_message_tokens(estimated_request);
    const int threshold = get_auto_compact_threshold(context_window);
    LOG_INFO("Auto-compact preflight; messages=" + std::to_string(history_.view().size()) +
             " current_request_estimated_tokens=" + std::to_string(pre_tokens) +
             " threshold=" + std::to_string(threshold) +
             " context_window=" + std::to_string(context_window) +
             " server_total_tokens=" +
             std::to_string(last_api_total_tokens_.load()));

    if (inputs.hooks) {
        auto fields = hooks_.common_fields(kCodexHookEventPreCompact, inputs.session);
        auto payload = build_compact_hook_payload(fields, "auto");
        auto outcome = hooks_.dispatch(inputs.hooks, kCodexHookEventPreCompact, "auto", payload);
        hooks_.apply(outcome);
        if (outcome.continue_false || outcome.blocked || outcome.denied) {
            transcript_.emit_transcript_system_message(inputs.session, "[Auto-compact] Stopped by hook.",
                make_system_notice_metadata("context_compact_stopped"));
            return false;
        }
    }

    const std::string compact_notice_id = generate_uuid_v7();
    events_.emit(SessionEventKind::AgentProgress, nlohmann::json{
        {"phase", "compacting"},
        {"label", "Compacting conversation"},
        {"started_at_ms", now_epoch_ms()},
    });
    transcript_.emit_transcript_system_message(inputs.session,
        "[Auto-compact] Context approaching limit, compacting...",
        make_compact_notice_metadata(compact_notice_id, "progress"));

    const auto& provider_snapshot = inputs.provider;
    if (!provider_snapshot) {
        LOG_WARN("Auto-compact failed; provider unavailable");
        transcript_.emit_transcript_system_message(inputs.session,
            "[Auto-compact] provider unavailable for compaction",
            make_compact_notice_metadata(compact_notice_id, "error", false, {{"provider_unavailable", true}}));
        return false;
    }

    LOG_INFO("Auto full compact starting; messages=" + std::to_string(history_.view().size()) +
             " active_estimated_tokens=" + std::to_string(pre_tokens) +
             " threshold=" + std::to_string(threshold));
    agent::ActiveProviderScope active_provider(active_provider_, provider_snapshot);
    CompactResult result = compact_messages(
        *provider_snapshot,
        history_.view(),
        initial_context,
        true,
        &abort_.flag_for_legacy_api(),
        [owner = lifetime_.ref(*this)](const ProviderErrorInfo& info, bool waiting) {
            owner.with([&info, waiting](CompactionController& controller) {
                controller.retry_.standard(info, waiting, true);
            });
        });
    active_provider.reset();

    if (!result.performed) {
        LOG_WARN("Auto full compact failed; error=" +
                 log_truncate(result.error, 500) +
                 " active_estimated_tokens=" + std::to_string(pre_tokens) +
                 " threshold=" + std::to_string(threshold));
        // 摘要压缩要发一次模型请求,于是它继承了那次请求的全部失败模式 ——
        // 网关超时、流中断、上游抽风。可上下文已经满了,这时候放弃等于让整个
        // 回合中止,用户只能重开会话。机械修剪不调用模型,因此不会以同样的方式
        // 失败;它保不住语义(丢的是最旧的整组消息,不是摘要),但能腾出空间让
        // 会话继续,这在"卡死"面前是明显更好的结果。
        if (mechanical_fallback(inputs, pre_tokens, context_window,
                                            compact_notice_id, result.error)) {
            return true;
        }
        transcript_.emit_transcript_system_message(inputs.session,
            "[Auto-compact] " + result.error,
            make_compact_notice_metadata(compact_notice_id, "error", false, {{"error", result.error}}));
        return false;
    }

    const int compacted_tokens = estimate_message_tokens(result.compacted_messages);
    LOG_INFO("Auto full compact succeeded; messages_before=" +
             std::to_string(history_.view().size()) +
             " messages_after=" + std::to_string(result.compacted_messages.size()) +
             " messages_compressed=" +
             std::to_string(result.messages_compressed) +
             " estimated_tokens_saved=" +
             std::to_string(result.estimated_tokens_saved) +
             " compacted_estimated_tokens=" + std::to_string(compacted_tokens));
    apply_result(inputs, result, "auto", compact_notice_id);
    if (inputs.hooks) {
        auto fields = hooks_.common_fields(kCodexHookEventPostCompact, inputs.session);
        auto payload = build_compact_hook_payload(fields, "auto");
        auto outcome = hooks_.dispatch(inputs.hooks, kCodexHookEventPostCompact, "auto", payload);
        hooks_.apply(outcome);
        if (outcome.continue_false) return false;
    }
    return true;
}

void CompactionController::run_manual(const CompactionInputs& inputs) {
    const auto callbacks = callbacks_.snapshot();
    abort_.clear();
    busy_ = true;

    const std::string compact_notice_id = generate_uuid_v7();

    if (inputs.session) {
        inputs.session->record_trajectory_event(
            "busy_changed", {{"busy", true}});
    }
    if (callbacks.on_busy_changed) {
        callbacks.on_busy_changed(true);
    }
    events_.emit(SessionEventKind::BusyChanged, nlohmann::json{{"busy", true}});
    events_.emit(SessionEventKind::AgentProgress, nlohmann::json{
        {"phase", "compacting"},
        {"label", "Compacting conversation"},
        {"started_at_ms", now_epoch_ms()},
    });
    transcript_.emit_transcript_system_message(inputs.session,
        "Compacting conversation...",
        make_compact_notice_metadata(compact_notice_id, "progress"));

    BusyCycleScope finish([owner = lifetime_.ref(*this), terminal = inputs.terminal] {
        owner.with([terminal](CompactionController& controller) { controller.finish_busy(terminal); });
    });

    if (inputs.hooks) {
        auto fields = hooks_.common_fields(kCodexHookEventPreCompact, inputs.session);
        auto payload = build_compact_hook_payload(fields, "manual");
        auto outcome = hooks_.dispatch(inputs.hooks, kCodexHookEventPreCompact, "manual", payload);
        hooks_.apply(outcome);
        if (outcome.continue_false || outcome.blocked || outcome.denied) {
            transcript_.emit_transcript_system_message(inputs.session, "[Compact] Stopped by hook.",
                make_system_notice_metadata("context_compact_stopped"));
            finish();
            return;
        }
    }

    const auto& provider_snapshot = inputs.provider;
    if (!provider_snapshot) {
        transcript_.dispatch_message("error", "[Error] provider unavailable for /compact", false,
                                     nlohmann::json::object(), nlohmann::json::array());
        finish();
        return;
    }

    agent::ActiveProviderScope active_provider(active_provider_, provider_snapshot);
    CompactResult result = compact_messages(
        *provider_snapshot,
        history_.view(),
        requests_.initial_context(inputs.request),
        false,
        &abort_.flag_for_legacy_api(),
        [owner = lifetime_.ref(*this)](const ProviderErrorInfo& info, bool waiting) {
            owner.with([&info, waiting](CompactionController& controller) {
                controller.retry_.standard(info, waiting, true);
            });
        });
    active_provider.reset();

    if (!result.performed) {
        transcript_.dispatch_message("error", "[Error] " + result.error, false,
                                     nlohmann::json::object(), nlohmann::json::array());
        finish();
        return;
    }

    apply_result(inputs, result, "manual", compact_notice_id);
    if (inputs.hooks) {
        auto fields = hooks_.common_fields(kCodexHookEventPostCompact, inputs.session);
        auto payload = build_compact_hook_payload(fields, "manual");
        auto outcome = hooks_.dispatch(inputs.hooks, kCodexHookEventPostCompact, "manual", payload);
        hooks_.apply(outcome);
        if (outcome.continue_false) {
            finish();
            return;
        }
    }

    finish();
}


void CompactionController::finish_busy(LifetimeRef<TrajectoryRecorder> terminal) {
    const auto callbacks = callbacks_.snapshot();
    terminal.with([](TrajectoryRecorder& recorder) {
        recorder.record_terminal({{"busy", false}}, nlohmann::json::object());
    });
    if (callbacks.on_busy_changed) callbacks.on_busy_changed(false);
    busy_ = false;
    events_.emit(SessionEventKind::BusyChanged, nlohmann::json{{"busy", false}});
    events_.emit(SessionEventKind::Done, nlohmann::json::object());
}

void CompactionController::mark_history_repaired() {
    compact_generation_.fetch_add(1, std::memory_order_relaxed);
    last_api_total_tokens_.store(0, std::memory_order_relaxed);
}

} // namespace acecode::agent
