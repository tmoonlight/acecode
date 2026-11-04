#include "pa_overflow_rescue.hpp"
#include "llm/context_thresholds.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

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
        case RescueAction::WithholdImages: return "withhold_images";
        case RescueAction::RetrySameRequest: return "retry_same_request";
        case RescueAction::CompactHistory: return "compact_history";
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

    // 0. 带内联图片被拒:先撤图片再立刻重发。排在一切之前 —— 原样重发同一张图
    //    只会被同样拒掉,收缩历史又清不到它(最近一条工具结果受保护,用户消息
    //    里的图更不会被清),小请求分支则会抱着它一直等。
    if (inputs.inline_images && !state.images_withheld) {
        plan.action = RescueAction::WithholdImages;
        plan.label = "服务端拒收了带图片的请求，改为文字引用后重试";
        return plan;
    }

    // Check before every mutation, including after an earlier large request
    // was reduced. A small rejected request is not evidence for a lower budget.
    if (inputs.request_tokens < MIN_AUTO_COMPACT_TOKENS) {
        plan.preserve_context = true;
        if (state.wait_retries < PA_RESCUE_MAX_WAIT_RETRIES) {
            plan.action = RescueAction::WaitAndRetry;
            plan.wait_ms = wait_retry_delay_ms(state.wait_retries);
            plan.label = "上下文不足 40000 tokens，保留完整请求，" +
                         seconds_text(plan.wait_ms) + "后重试（第 " +
                         std::to_string(state.wait_retries + 1) + "/" +
                         std::to_string(PA_RESCUE_MAX_WAIT_RETRIES) + " 次）";
            return plan;
        }
        plan.action = RescueAction::GiveUp;
        plan.label = "上下文不足 40000 tokens 的请求在 " +
                     std::to_string(PA_RESCUE_MAX_WAIT_RETRIES) +
                     " 次等待重试后仍被拒收";
        return plan;
    }

    // 1. 原样重发。只在 episode 刚开始(还没压缩、还没缩过)且被拒的是正常档请求
    //    时做:已经压缩或缩过还被拒,说明不是抽风;紧急档被拒同理。
    if (state.shrink_rounds == 0 && !state.compaction_attempted &&
        !inputs.emergency_profile &&
        state.same_request_retries < PA_RESCUE_SAME_REQUEST_RETRIES) {
        plan.action = RescueAction::RetrySameRequest;
        plan.wait_ms = same_request_retry_delay_ms(state.same_request_retries);
        plan.label = "服务端拒收请求，" + seconds_text(plan.wait_ms) +
                     "后原样重发（第 " +
                     std::to_string(state.same_request_retries + 1) + "/" +
                     std::to_string(PA_RESCUE_SAME_REQUEST_RETRIES) + " 次）";
        return plan;
    }

    // 压缩过的 episode 按压缩前那次确认拒收的规模记账,否则按被拒请求本身。
    const int rejected_tokens = state.confirmed_rejected_tokens > 0
        ? state.confirmed_rejected_tokens
        : inputs.request_tokens;

    // 2. 先压缩。拒收确认了,也只说明这次请求太大 —— 多半是历史长到了墙上,
    //    不说明服务端的上限比声明窗口小。先做一次摘要压缩再重发,不记账;
    //    压缩失败(调用方立刻换下一招)或压缩后的请求仍被拒,才收缩并记账。
    //    以前这里直接收缩并记账,一次撞墙就把压缩阈值砍下去,之后整个进程里
    //    这个模型都提前压缩,而那堵墙本来一次压缩就能绕过去。
    if (!state.compaction_attempted && state.shrink_rounds == 0) {
        plan.action = RescueAction::CompactHistory;
        // 带图请求的规模不当证据(图片开销估不准):压缩把图片去掉之后再被拒,
        // 就按那次不带图的规模记。
        plan.rejected_tokens = inputs.inline_images ? 0 : inputs.request_tokens;
        plan.label = "服务端确认拒收，先压缩会话历史后重试";
        return plan;
    }

    // 3. 逐档收缩。目标按整个请求的 85% 算,再扣掉历史之外的固定部分
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
        plan.record_rejection = !state.rejection_recorded &&
            !inputs.emergency_profile && !inputs.inline_images;
        plan.rejected_tokens = rejected_tokens;
        plan.label = "服务端确认拒收，收缩会话历史到约 " +
                     std::to_string(plan.target_history_tokens) +
                     " tokens 后重试（第 " +
                     std::to_string(state.shrink_rounds + 1) + " 次收缩）";
        return plan;
    }

    // 4. 紧急档。历史已无可缩,只剩固定部分可剥。
    if (!inputs.emergency_profile) {
        plan.action = RescueAction::EmergencyProfile;
        plan.record_rejection =
            !state.rejection_recorded && !inputs.inline_images;
        plan.rejected_tokens = rejected_tokens;
        plan.label = "会话历史已无可缩，改用精简请求档（仅核心工具）重试";
        return plan;
    }

    // 5. 等待重发。已经是最小请求了,只能等服务端恢复。
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
        case RescueAction::CompactHistory:
            state.compaction_attempted = true;
            state.confirmed_rejected_tokens = plan.rejected_tokens;
            break;
        case RescueAction::ShrinkHistory:
            ++state.shrink_rounds;
            break;
        case RescueAction::WaitAndRetry:
            if (state.wait_retries < std::numeric_limits<int>::max()) {
                ++state.wait_retries;
            }
            break;
        case RescueAction::WithholdImages:
            state.images_withheld = true;
            break;
        case RescueAction::EmergencyProfile:
        case RescueAction::GiveUp:
            break;
    }
}

}  // namespace acecode::pa
