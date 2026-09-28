#include "tui/app/model_pool_monitor_subscription.hpp"
#include "provider/model_pool_status.hpp"
#include "provider/session_model_binding.hpp"
#include "agent/agent_loop.hpp"
#include "tui/render/status_chips.hpp"
namespace acecode::tui {
ModelPoolMonitorSubscription::ModelPoolMonitorSubscription(SessionModelBinding& binding,
    AppConfig& config, AgentLoop& agent, std::weak_ptr<UiPostTarget> target)
    : binding_(binding), config_(config), agent_(agent), target_(std::move(target)) {
    if (should_start_model_pool_monitor(config_.saved_models)) {
        model_pool_status_service().start([ref = lifetime_.ref(*this)] {
            ref.with([](ModelPoolMonitorSubscription& owner) { owner.changed(); });
        });
    }
}
ModelPoolMonitorSubscription::~ModelPoolMonitorSubscription() { stop(); }
void ModelPoolMonitorSubscription::stop() {
    if (stopped_) return;
    model_pool_status_service().stop();
    lifetime_.revoke();
    stopped_ = true;
}
void ModelPoolMonitorSubscription::changed() {
    std::string model_id;
    if (auto provider = binding_.provider_snapshot()) model_id = provider->model();
    int percent = -1, effective_window = 0;
    if (auto status = model_pool_status_service().get(model_id)) {
        percent = status->usage_rate;
        effective_window = effective_context_window(status->max_window_tokens);
    }
    g_model_load_percent.store(percent);
    auto target = target_.lock();
    if (!target) return;
    if (effective_window > 0) {
        target->post_task([ref = lifetime_.ref(*this), effective_window] {
            ref.with([&](ModelPoolMonitorSubscription& owner) {
                if (owner.config_.context_window != effective_window) {
                    owner.config_.context_window = effective_window;
                    owner.agent_.set_context_window(effective_window);
                }
            });
        });
    }
    target->post_event(ftxui::Event::Custom);
}
}
