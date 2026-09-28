#include "pa_overflow_rescue.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace acecode::pa {
namespace {

std::atomic<double> g_wait_scale{1.0};

std::string seconds_text(int ms) {
    if (ms % 1000 == 0) return std::to_string(ms / 1000) + " 秒";
    return std::to_string(ms / 1000.0) + " 秒";
}

}  // namespace

int same_request_retry_delay_ms(int attempt) {
    return attempt <= 0 ? 2000 : 5000;
}

int wait_retry_delay_ms(int attempt) {
    long long delay = PA_RESCUE_WAIT_BASE_MS;
    for (int i = 0; i < attempt && delay < PA_RESCUE_WAIT_MAX_MS; ++i) {
        delay *= 2;
    }
    return static_cast<int>(
        std::min<long long>(delay, PA_RESCUE_WAIT_MAX_MS));
}

void set_rescue_wait_scale_for_test(double scale) {
    g_wait_scale.store(scale < 0.0 ? 0.0 : scale);
}

int scaled_rescue_wait_ms(int ms) {
    const double scale = g_wait_scale.load();
    if (ms <= 0 || scale <= 0.0) return 0;
    return static_cast<int>(std::lround(static_cast<double>(ms) * scale));
}

const char* to_string(RescueAction action) {
    switch (action) {
        case RescueAction::RetrySameRequest: return "retry_same_request";
        case RescueAction::ShrinkHistory: return "shrink_history";
        case RescueAction::EmergencyProfile: return "emergency_profile";
        case RescueAction::WaitAndRetry: return "wait_and_retry";
        case RescueAction::GiveUp: return "give_up";
    }
    return "give_up";
}

RescuePlan next_rescue_step(const RescueState& state,
                            const RescueInputs& inputs) {
    RescuePlan plan;

    // 1. 原样重发。只在 episode 刚开始(还没缩过)且被拒的是正常档请求时做:
    //    已经缩过还被拒,说明不是抽风;紧急档被拒同理。
    if (state.shrink_rounds == 0 && !inputs.emergency_profile &&
        state.same_request_retries < PA_RESCUE_SAME_REQUEST_RETRIES) {
        plan.action = RescueAction::RetrySameRequest;
        plan.wait_ms = same_request_retry_delay_ms(state.same_request_retries);
        plan.label = "服务端拒收请求，" + seconds_text(plan.wait_ms) +
                     "后原样重发（第 " +
                     std::to_string(state.same_request_retries + 1) + "/" +
                     std::to_string(PA_RESCUE_SAME_REQUEST_RETRIES) + " 次）";
        return plan;
    }

    // 2. 逐档收缩。目标按整个请求的 85% 算,再扣掉历史之外的固定部分
    //    (system prompt、注入上下文),得到历史应缩到的规模。
    if (!state.shrink_exhausted) {
        plan.action = RescueAction::ShrinkHistory;
        const long long target_total = std::max<long long>(
            1, static_cast<long long>(inputs.request_tokens) *
                   PA_RESCUE_SHRINK_PERCENT / 100);
        const long long fixed = std::max<long long>(
            0, static_cast<long long>(inputs.request_tokens) -
                   inputs.history_tokens);
        plan.target_history_tokens =
            static_cast<int>(std::max<long long>(1, target_total - fixed));
        plan.record_rejection =
            !state.rejection_recorded && !inputs.emergency_profile;
        plan.label = "服务端确认拒收，收缩会话历史到约 " +
                     std::to_string(plan.target_history_tokens) +
                     " tokens 后重试（第 " +
                     std::to_string(state.shrink_rounds + 1) + " 次收缩）";
        return plan;
    }

    // 3. 紧急档。历史已无可缩,只剩固定部分可剥。
    if (!inputs.emergency_profile) {
        plan.action = RescueAction::EmergencyProfile;
        plan.record_rejection = !state.rejection_recorded;
        plan.label = "会话历史已无可缩，改用精简请求档（仅核心工具）重试";
        return plan;
    }

    // 4. 等待重发。已经是最小请求了,只能等服务端恢复。
    if (state.wait_retries < PA_RESCUE_MAX_WAIT_RETRIES) {
        plan.action = RescueAction::WaitAndRetry;
        plan.wait_ms = wait_retry_delay_ms(state.wait_retries);
        plan.label = "服务端仍拒收最小请求，" + seconds_text(plan.wait_ms) +
                     "后重试（第 " + std::to_string(state.wait_retries + 1) +
                     "/" + std::to_string(PA_RESCUE_MAX_WAIT_RETRIES) +
                     " 次等待）";
        return plan;
    }

    plan.action = RescueAction::GiveUp;
    plan.label = "服务端在 " + std::to_string(PA_RESCUE_MAX_WAIT_RETRIES) +
                 " 次等待重试后仍拒收最小请求";
    return plan;
}

void advance_rescue_state(RescueState& state, const RescuePlan& plan) {
    state.active = true;
    if (plan.record_rejection) state.rejection_recorded = true;
    switch (plan.action) {
        case RescueAction::RetrySameRequest:
            ++state.same_request_retries;
            break;
        case RescueAction::ShrinkHistory:
            ++state.shrink_rounds;
            break;
        case RescueAction::WaitAndRetry:
            ++state.wait_retries;
            break;
        case RescueAction::EmergencyProfile:
        case RescueAction::GiveUp:
            break;
    }
}

}  // namespace acecode::pa
