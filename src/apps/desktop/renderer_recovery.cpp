#include "renderer_recovery.hpp"

namespace acecode::desktop {

RendererRecoveryTracker::RendererRecoveryTracker(RendererRecoveryLimits limits)
    : limits_(limits) {}

RendererRecoveryAction RendererRecoveryTracker::on_failure(RendererFailure failure,
                                                           std::int64_t now_ms) {
    if (failure != RendererFailure::RenderProcessExited) {
        return RendererRecoveryAction::Ignore;
    }
    return admit_reload(now_ms) ? RendererRecoveryAction::Reload : RendererRecoveryAction::GiveUp;
}

bool RendererRecoveryTracker::admit_reload(std::int64_t now_ms) {
    while (!reloads_ms_.empty() && now_ms - reloads_ms_.front() >= limits_.window_ms) {
        reloads_ms_.pop_front();
    }
    if (static_cast<int>(reloads_ms_.size()) >= limits_.max_reloads) return false;
    reloads_ms_.push_back(now_ms);
    return true;
}

} // namespace acecode::desktop
