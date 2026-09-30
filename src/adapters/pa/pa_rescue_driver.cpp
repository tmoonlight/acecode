#include "pa_rescue_driver.hpp"
#include "session/system_notice.hpp"
#include "utils/logger.hpp"

namespace acecode::pa {
namespace {
void emit_retry(PaRescueHost& host, const ProviderErrorInfo& error, const RescuePlan& plan,
                int attempt, int max_attempts, bool waiting) {
    ProviderErrorInfo info = error;
    info.retry_attempt = attempt;
    info.retry_max_attempts = max_attempts;
    info.retry_delay_ms = waiting ? scaled_rescue_wait_ms(plan.wait_ms) : 0;
    host.retry(info, waiting,
        waiting ? plan.label : std::string("正在重新发送请求"),
        waiting ? std::string("服务端报「请求上下文过大」，按 PA 兜底策略等待后重发")
                : std::string{});
}
}
bool run_rescue(PaRescueHost& host, RescueState& state, const ProviderErrorInfo& error,
                int request_tokens, bool& emergency_request_profile) {
    if (!state.active) state = pa::RescueState{};
    const int history_tokens = host.history_tokens();

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
        if (plan.record_rejection) host.note_rejection(request_tokens);

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
                    host.notice(
                        "[智能压缩] 服务端报「请求上下文过大」，先原样重发确认"
                        "是否为瞬时故障；确认拒收后才会收缩历史。",
                        make_system_notice_metadata("context_retrying"));
                } else if (waiting_for_recovery && state.wait_retries == 1) {
                    host.notice(
                        "[智能压缩] 请求已缩到最小仍被服务端拒收；将按 5 秒起、"
                        "最长 60 秒的间隔反复重试（最多 " +
                        std::to_string(pa::PA_RESCUE_MAX_WAIT_RETRIES) +
                        " 次），可随时停止。", make_system_notice_metadata("context_waiting",
                            {{"attempts", pa::PA_RESCUE_MAX_WAIT_RETRIES}}));
                }
                emit_retry(host,
                    error, plan, attempt, max_attempts, true);
                if (!host.wait(scaled_rescue_wait_ms(plan.wait_ms))) {
                    return false;
                }
                emit_retry(host,
                    error, plan, attempt, max_attempts, false);
                host.reset_stream();
                return true;
            }
            case pa::RescueAction::ShrinkHistory: {
                ThreadRepairOptions options;
                options.trigger = "repair-pa-overflow";
                options.target_tokens = plan.target_history_tokens;
                options.force_prune_one_group = true;
                options.clear_tool_outputs = true;
                options.keep_recent_tool_outputs = 1;
                options.thin_old_turns_first = true;
                auto repair = host.repair(options);
                LOG_WARN("[pa-rescue] shrink status=" +
                         std::string(to_string(repair.status)) +
                         " pre_tokens=" + std::to_string(repair.pre_tokens) +
                         " post_tokens=" + std::to_string(repair.post_tokens) +
                         " pruned_groups=" +
                         std::to_string(repair.pruned_groups) +
                         " cleared_tool_outputs=" +
                         std::to_string(repair.cleared_tool_outputs) +
                         " thinned_groups=" +
                         std::to_string(repair.thinned_groups) +
                         " reason=" + repair.reason);
                if (!repair.repaired()) {
                    // 一点空间都没腾出来:这一轮不再提议收缩,立刻换下一招。
                    state.shrink_exhausted = true;
                    continue;
                }
                host.history_repaired();
                host.reset_stream();
                host.progress(nlohmann::json{
                    {"phase", "context_repair"},
                    {"label", plan.label},
                    {"detail", repair.reason},
                });
                if (repair.thinned_groups > 0) {
                    host.notice(
                        "[智能压缩] 服务端拒收请求（第 " +
                        std::to_string(state.shrink_rounds) +
                        " 次收缩）：已清除 " +
                        std::to_string(repair.cleared_tool_outputs) +
                        " 条旧工具输出、精简 " +
                        std::to_string(repair.thinned_groups) +
                        " 组旧回合（保留用户消息与结论）、丢弃 " +
                        std::to_string(repair.pruned_groups) +
                        " 组最旧历史后重试。",
                        make_system_notice_metadata("context_history_thinned",
                            {{"round", state.shrink_rounds},
                             {"groups", repair.pruned_groups},
                             {"thinned", repair.thinned_groups},
                             {"outputs", repair.cleared_tool_outputs}}));
                    return true;
                }
                host.notice(
                    "[智能压缩] 服务端拒收请求（第 " +
                    std::to_string(state.shrink_rounds) +
                    " 次收缩）：已丢弃最旧的 " +
                    std::to_string(repair.pruned_groups) + " 组历史、清除 " +
                    std::to_string(repair.cleared_tool_outputs) +
                    " 条旧工具输出后重试。", make_system_notice_metadata("context_history_pruned",
                        {{"round", state.shrink_rounds}, {"groups", repair.pruned_groups},
                         {"outputs", repair.cleared_tool_outputs}}));
                return true;
            }
            case pa::RescueAction::EmergencyProfile: {
                emergency_request_profile = true;
                host.reset_stream();
                host.progress(nlohmann::json{
                    {"phase", "context_repair"},
                    {"label", plan.label},
                    {"detail", "去掉工具定义与注入上下文，仅保留核心工具"},
                });
                host.notice("[智能压缩] " + plan.label + "。",
                    make_system_notice_metadata("context_emergency"));
                return true;
            }
            case pa::RescueAction::GiveUp:
                LOG_WARN("[pa-rescue] giving up: " + plan.label);
                return false;
        }
    }
}

} // namespace acecode::pa
