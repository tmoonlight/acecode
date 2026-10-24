#include "session_manager.hpp"
#include "session_usage_ledger.hpp"

#include <utility>

namespace acecode {

bool SessionManager::set_active_provider(const std::string& provider,
                                         const std::string& model) {
    return set_active_provider(provider, model, std::string{});
}

bool SessionManager::set_active_provider(const std::string& provider,
                                         const std::string& model,
                                         const std::string& model_preset) {
    std::lock_guard<std::mutex> lk(mu_);
    if (provider_name_ != provider || model_name_ != model || model_preset_ != model_preset) {
        model_activated_at_ms_ = usage_now_unix_ms();
    }
    provider_name_ = provider;
    model_name_ = model;
    if (model_preset_ != model_preset) reasoning_effort_.reset();
    model_preset_ = model_preset;
    if (created_) {
        return update_meta();
    }
    publish_display_snapshot_locked();
    return true;
}

bool SessionManager::set_active_model_state(
    const std::string& provider,
    const std::string& model,
    const std::string& model_preset,
    const std::optional<std::string>& reasoning_effort,
    bool persist_immediately) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto old_provider = provider_name_;
    const auto old_model = model_name_;
    const auto old_preset = model_preset_;
    const auto old_effort = reasoning_effort_;
    const auto old_activated_at = model_activated_at_ms_;
    if (!persist_immediately && old_provider == provider && old_model == model &&
        old_preset == model_preset && old_effort == reasoning_effort) return true;
    provider_name_ = provider;
    model_name_ = model;
    model_preset_ = model_preset;
    if (old_provider != provider || old_model != model || old_preset != model_preset) {
        model_activated_at_ms_ = usage_now_unix_ms();
    }
    reasoning_effort_ = reasoning_effort;
    bool written = false;
    try {
        if (persist_immediately && !created_) ensure_created();
        written = created_ ? update_meta() : !persist_immediately;
    } catch (...) {
        written = false;
    }
    if (!written) {
        provider_name_ = old_provider;
        model_name_ = old_model;
        model_preset_ = old_preset;
        reasoning_effort_ = old_effort;
        model_activated_at_ms_ = old_activated_at;
        // If initial materialization succeeded before a later write failed,
        // make a best-effort attempt to restore the durable selection too.
        try { if (created_) (void)update_meta(); } catch (...) {}
    }
    publish_display_snapshot_locked();
    return written;
}

bool SessionManager::set_sampling_overrides(ModelSamplingOptions sampling, bool persist_immediately) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto previous = sampling_;
    sampling_ = std::move(sampling);
    bool written = false;
    try {
        if (persist_immediately && !created_) ensure_created();
        written = created_ ? update_meta() : !persist_immediately;
    } catch (...) {
        written = false;
    }
    if (!written) {
        sampling_ = previous;
        try { if (created_) (void)update_meta(); } catch (...) {}
    }
    return written;
}

ModelSamplingOptions SessionManager::current_sampling_overrides() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sampling_;
}

std::string SessionManager::current_model_preset() const {
    std::lock_guard<std::mutex> lk(mu_);
    return model_preset_;
}

} // namespace acecode
