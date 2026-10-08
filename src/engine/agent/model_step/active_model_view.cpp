#include "active_model_view.hpp"
#include "llm/token_estimate.hpp"
#include "llm/model_family.hpp"
#include "llm/context_thresholds.hpp"
#include "pa/pa_context_budget.hpp"
#include "session/system_notice.hpp"
#include "session/token_tracker.hpp"
#include "utils/logger.hpp"
#include <utility>

namespace acecode::agent {

ActiveModelView::ActiveModelView(std::shared_ptr<LlmProvider> provider, int declared_window, AgentRuntimeEnv environment)
    : provider_(std::move(provider)), declared_window_(declared_window), environment_(std::move(environment)) {
    if (provider_) {
        provider_name_ = provider_->name();
        model_name_ = provider_->model();
    }
}
int ActiveModelView::effective_window() const {
    return environment_.context_budget().effective_window(provider_name_, model_name_, declared_window_);
}
bool ActiveModelView::can_read_images() const {
    return !provider_ || provider_->supports_vision();
}
SystemPromptModelState ActiveModelView::prompt_state() const {
    return {model_name_, detect_model_family(model_name_), model_prefers_apply_patch(model_name_)};
}

std::optional<ContextRejectionNotice> ActiveModelView::note_rejected(int request_tokens) const {
    // 与 learner 内部同一道门:不可信的规模在这里就退,省掉 provider 快照与
    // 两次查表。口径必须一致,否则「记了但没生效」会看起来像 bug。
    if (!pa::observation_is_credible(request_tokens)) return std::nullopt;
    const std::string& provider = provider_name_;
    const std::string& model = model_name_;
    const int declared = declared_window_;
    auto& budget = environment_.context_budget();
    const int before = budget.effective_window(provider, model, declared);
    budget.note_rejected(provider, model, request_tokens);
    const int after = budget.effective_window(provider, model, declared);
    const int threshold = get_auto_compact_threshold(after);
    if (threshold >= get_auto_compact_threshold(before)) return std::nullopt;

    LOG_WARN("[pa] context rejection observed; request_estimated_tokens=" +
             std::to_string(request_tokens) +
             " declared_window=" + std::to_string(declared) +
             " compaction_window_before=" + std::to_string(before) +
             " compaction_window_after=" + std::to_string(after) +
             " provider=" + provider + " model=" + model);

    // 让用户看得见适配层做了什么。收敛是单调的,所以这条提示最多出现几次,
    // 不会刷屏;不提示的话用户只会觉得「压缩怎么突然变频繁了」。
    return ContextRejectionNotice{
        "[智能压缩] 服务端在约 " + std::to_string(request_tokens) +
        " tokens (最大 " + std::to_string(declared) +
        " tokens) 处拒收了请求，压缩阈值下调至 " + std::to_string(threshold) +
        " tokens", make_system_notice_metadata("context_threshold_lowered",
            {{"tokens", request_tokens}, {"declared", declared}, {"threshold", threshold}})};
}

void ActiveModelView::note_accepted(
    const std::vector<ChatMessage>& messages_with_system) const {
    const std::string& provider = provider_name_;
    const std::string& model = model_name_;
    auto& budget = environment_.context_budget();
    if (!budget.has_observation(provider, model)) return;
    budget.note_accepted(provider, model,
                         estimate_message_tokens(messages_with_system));
}

} // namespace acecode::agent
