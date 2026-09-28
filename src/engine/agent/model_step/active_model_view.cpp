#include "agent/agent_loop.hpp"
#include "llm/model_family.hpp"
#include "llm/tool_protocol_names.hpp"
#include "pa/pa_context_budget.hpp"
#include "pa/pa_overflow_rescue.hpp"
#include "permissions/shell_write_guard.hpp"
#include "prompt/system_prompt.hpp"
#include "session/session_client.hpp"
#include "session/system_notice.hpp"
#include "session/thread_goal_store.hpp"
#include "session/thread_repair.hpp"
#include "session/token_tracker.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode {

void AgentLoop::active_model_identity(std::string& provider,
                                      std::string& model) const {
    provider.clear();
    model.clear();
    if (!provider_accessor_) return;
    const std::shared_ptr<LlmProvider> snapshot = provider_accessor_();
    if (!snapshot) return;
    provider = snapshot->name();
    model = snapshot->model();
}

int AgentLoop::compaction_context_window() const {
    const int declared = context_window_.load(std::memory_order_relaxed);
    std::string provider;
    std::string model;
    active_model_identity(provider, model);
    return pa::context_budget().effective_window(provider, model, declared);
}

void AgentLoop::note_pa_context_rejection(int request_tokens) {
    // 与 learner 内部同一道门:不可信的规模在这里就退,省掉 provider 快照与
    // 两次查表。口径必须一致,否则「记了但没生效」会看起来像 bug。
    if (!pa::observation_is_credible(request_tokens)) return;
    std::string provider;
    std::string model;
    active_model_identity(provider, model);
    const int declared = context_window_.load(std::memory_order_relaxed);
    auto& budget = pa::context_budget();
    const int before = budget.effective_window(provider, model, declared);
    budget.note_rejected(provider, model, request_tokens);
    const int after = budget.effective_window(provider, model, declared);
    if (after >= before) return;

    LOG_WARN("[pa] context rejection observed; request_estimated_tokens=" +
             std::to_string(request_tokens) +
             " declared_window=" + std::to_string(declared) +
             " compaction_window_before=" + std::to_string(before) +
             " compaction_window_after=" + std::to_string(after) +
             " provider=" + provider + " model=" + model);

    // 让用户看得见适配层做了什么。收敛是单调的,所以这条提示最多出现几次,
    // 不会刷屏;不提示的话用户只会觉得「压缩怎么突然变频繁了」。
    emit_transcript_system_message(
        "[智能压缩] 服务端在约 " + std::to_string(request_tokens) +
        " tokens (最大 " + std::to_string(declared) +
        " tokens) 处拒收了请求，压缩阈值下调至 " + std::to_string(after) +
        " tokens", make_system_notice_metadata("context_threshold_lowered",
            {{"tokens", request_tokens}, {"declared", declared}, {"threshold", after}}));
}

void AgentLoop::note_pa_context_accepted(
    const std::vector<ChatMessage>& messages_with_system) {
    std::string provider;
    std::string model;
    active_model_identity(provider, model);
    auto& budget = pa::context_budget();
    if (!budget.has_observation(provider, model)) return;
    budget.note_accepted(provider, model,
                         estimate_message_tokens(messages_with_system));
}

bool AgentLoop::active_model_can_read_images() const {
    if (!provider_accessor_) return true;
    const std::shared_ptr<LlmProvider> provider = provider_accessor_();
    if (!provider) return true;
    return provider->supports_vision();
}

SystemPromptModelState AgentLoop::system_prompt_model_state() const {
    SystemPromptModelState state;
    std::string provider_name;
    active_model_identity(provider_name, state.model_id);
    state.family = detect_model_family(state.model_id);
    state.prefers_apply_patch = model_prefers_apply_patch(state.model_id);
    return state;
}

} // namespace acecode
