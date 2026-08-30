#include "session_model_binding.hpp"

#include "copilot_provider.hpp"
#include "model_context_resolver.hpp"
#include "model_pool_status.hpp"
#include "../utils/logger.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace acecode {
namespace {

constexpr int kMaxStaleBuildRetries = 8;

bool is_session_local_profile(const std::string& name) {
    return name.rfind("(session:", 0) == 0;
}

std::string authenticate_after_construction(
    const std::shared_ptr<LlmProvider>& provider) {
    if (!provider || provider->name() != "copilot") return {};
    try {
        auto copilot = std::dynamic_pointer_cast<CopilotProvider>(provider);
        if (copilot && copilot->try_silent_auth()) return {};
    } catch (...) {
        // The public warning is intentionally fixed below.
    }
    return "Copilot silent authentication failed; sign in before the next request";
}

} // namespace

const char* to_string(SessionModelReloadOutcome outcome) noexcept {
    switch (outcome) {
        case SessionModelReloadOutcome::Reloaded:
            return "reloaded";
        case SessionModelReloadOutcome::AlreadyCurrent:
            return "already_current";
        case SessionModelReloadOutcome::Unresolvable:
            return "unresolvable";
    }
    return "already_current";
}

SessionModelState session_model_state_from_profile(
    const AppConfig& config,
    const ModelProfile& profile) {
    SessionModelState state;
    state.name = profile.name;
    state.provider = profile.provider;
    state.model = profile.model;
    const int effective_pool_window =
        model_pool_status_service().effective_context_window_for(state.model);
    state.context_window = resolve_runtime_model_profile_context_window_nonblocking(
        config, profile, config.context_window, effective_pool_window);
    return state;
}

std::shared_ptr<LlmProvider> SessionModelBinding::provider_snapshot() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return provider_;
}

SessionModelState SessionModelBinding::state_snapshot() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return state_;
}

SavedModelsRevision SessionModelBinding::applied_revision() const noexcept {
    return applied_revision_.load(std::memory_order_acquire);
}

void SessionModelBinding::append_warning(std::string& warning,
                                         const std::string& addition) {
    if (addition.empty()) return;
    if (!warning.empty()) warning += "; ";
    warning += addition;
}

SessionModelReloadResult SessionModelBinding::already_current_result() const {
    SessionModelReloadResult result;
    result.outcome = SessionModelReloadOutcome::AlreadyCurrent;
    result.state = state_snapshot();
    return result;
}

SessionModelReloadResult SessionModelBinding::unresolvable_result(
    SavedModelsRevision observed_revision,
    bool record_revision) {
    if (record_revision) {
        const auto before = applied_revision_.load(std::memory_order_acquire);
        if (observed_revision >= before) {
            applied_revision_.store(observed_revision, std::memory_order_release);
        }
    }
    SessionModelReloadResult result;
    result.outcome = SessionModelReloadOutcome::Unresolvable;
    result.state = state_snapshot();
    return result;
}

SessionModelReloadResult SessionModelBinding::install_explicit(
    SessionModelResolvedTarget target,
    const SessionModelResolver& resolver,
    const SessionModelTransitionCallback& on_transition) {
    std::lock_guard<std::mutex> operation_lock(operation_mu_);
    std::uint64_t generation = 0;
    std::string selected_name = target.state.name;
    if (selected_name.empty() && target.profile.has_value()) {
        selected_name = target.profile->name;
        target.state.name = selected_name;
    }
    {
        std::lock_guard<std::mutex> state_lock(state_mu_);
        generation = ++selected_generation_;
    }
    return install_target_locked(
        std::move(target), selected_name, generation, true,
        resolver, on_transition);
}

SessionModelReloadResult SessionModelBinding::ensure_current(
    bool force,
    const SessionModelRevisionAccessor& current_revision,
    const SessionModelResolver& resolver,
    const SessionModelTransitionCallback& on_transition) {
    if (!current_revision || !resolver) {
        SessionModelReloadResult result;
        result.ok = false;
        result.state = state_snapshot();
        result.error = "model reload is unavailable";
        return result;
    }

    SavedModelsRevision observed = current_revision();
    if (!force && observed == applied_revision()) {
        return already_current_result();
    }

    std::lock_guard<std::mutex> operation_lock(operation_mu_);
    observed = current_revision();
    if (!force && observed == applied_revision()) {
        return already_current_result();
    }

    std::string selected_name;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> state_lock(state_mu_);
        selected_name = state_.name;
        generation = selected_generation_;
    }

    SessionModelResolvedTarget target;
    try {
        target = resolver(selected_name);
    } catch (...) {
        SessionModelReloadResult result;
        result.ok = false;
        result.state = state_snapshot();
        result.error = "model profile resolution failed";
        return result;
    }
    return install_target_locked(
        std::move(target), selected_name, generation, false,
        resolver, on_transition);
}

SessionModelReloadResult SessionModelBinding::install_target_locked(
    SessionModelResolvedTarget target,
    const std::string& selected_name,
    std::uint64_t selected_generation,
    bool explicit_install,
    const SessionModelResolver& resolver,
    const SessionModelTransitionCallback& on_transition) {
    for (int attempt = 0; attempt < kMaxStaleBuildRetries; ++attempt) {
        if (!target.profile.has_value() || !target.config) {
            return unresolvable_result(target.revision, !explicit_install);
        }

        std::optional<PreparedProviderConstruction> prepared;
        try {
            prepared = prepare_provider_construction(
                *target.profile, target.config.get());
        } catch (...) {
            SessionModelReloadResult result;
            result.ok = false;
            result.state = state_snapshot();
            result.error = "provider construction plan failed";
            return result;
        }
        if (!prepared.has_value()) {
            SessionModelReloadResult result;
            result.ok = false;
            result.state = state_snapshot();
            result.error = "provider construction plan is invalid";
            return result;
        }

        bool fingerprint_matches = false;
        bool selection_changed = false;
        {
            std::lock_guard<std::mutex> state_lock(state_mu_);
            if (selected_generation_ != selected_generation) {
                SessionModelReloadResult result;
                result.ok = false;
                result.state = state_;
                result.error = "model selection changed during reload";
                return result;
            }
            fingerprint_matches = fingerprint_.has_value() &&
                *fingerprint_ == prepared->fingerprint();
            selection_changed = state_.name != target.state.name;
        }

        if (fingerprint_matches) {
            SessionModelResolvedTarget latest;
            try {
                latest = explicit_install && is_session_local_profile(selected_name)
                    ? target
                    : (resolver ? resolver(selected_name) : target);
            } catch (...) {
                SessionModelReloadResult result;
                result.ok = false;
                result.state = state_snapshot();
                result.error = "model profile revalidation failed";
                return result;
            }
            if (!latest.profile.has_value() || !latest.config) {
                return unresolvable_result(latest.revision, !explicit_install);
            }
            std::optional<PreparedProviderConstruction> latest_prepared;
            try {
                latest_prepared = prepare_provider_construction(
                    *latest.profile, latest.config.get());
            } catch (...) {
                SessionModelReloadResult result;
                result.ok = false;
                result.state = state_snapshot();
                result.error =
                    "provider construction plan failed after revalidation";
                return result;
            }
            if (!latest_prepared.has_value()) {
                SessionModelReloadResult result;
                result.ok = false;
                result.state = state_snapshot();
                result.error =
                    "provider construction plan is invalid after revalidation";
                return result;
            }
            if (latest_prepared->fingerprint() != prepared->fingerprint()) {
                target = std::move(latest);
                continue;
            }
            target = std::move(latest);

            bool stale_revision = false;
            {
                std::lock_guard<std::mutex> state_lock(state_mu_);
                if (selected_generation_ != selected_generation) continue;
                const auto before = applied_revision_.load(std::memory_order_acquire);
                if (target.revision < before) {
                    stale_revision = true;
                } else {
                    state_ = target.state;
                    applied_revision_.store(target.revision,
                                            std::memory_order_release);
                }
            }
            if (stale_revision) continue;
            SessionModelReloadResult result;
            result.outcome = SessionModelReloadOutcome::AlreadyCurrent;
            result.state = target.state;
            if (on_transition) {
                try {
                    if (!on_transition(
                            result.state,
                            SessionModelTransition{false, selection_changed})) {
                        append_warning(
                            result.warning,
                            "session metadata could not be persisted");
                    }
                } catch (...) {
                    append_warning(
                        result.warning,
                        "session metadata could not be persisted");
                }
            }
            return result;
        }

        std::optional<ProviderConstructionResult> construction;
        try {
            construction.emplace(prepared->construct());
        } catch (...) {
            SessionModelReloadResult result;
            result.ok = false;
            result.state = state_snapshot();
            result.error = "provider construction failed";
            return result;
        }
        if (!construction->provider) {
            SessionModelReloadResult result;
            result.ok = false;
            result.state = state_snapshot();
            result.error = "provider construction failed";
            return result;
        }
        std::string warning =
            authenticate_after_construction(construction->provider);

        SessionModelResolvedTarget latest;
        try {
            // Ad-hoc profiles reconstructed from session metadata are not
            // members of saved_models. They must still be installable for the
            // initial resume, while later revision-driven reloads deliberately
            // resolve them as unresolvable and retain this provider snapshot.
            latest = explicit_install && is_session_local_profile(selected_name)
                ? target
                : (resolver ? resolver(selected_name) : target);
        } catch (...) {
            SessionModelReloadResult result;
            result.ok = false;
            result.state = state_snapshot();
            result.error = "model profile revalidation failed";
            return result;
        }
        if (!latest.profile.has_value() || !latest.config) {
            return unresolvable_result(latest.revision, !explicit_install);
        }
        std::optional<PreparedProviderConstruction> latest_prepared;
        try {
            latest_prepared = prepare_provider_construction(
                *latest.profile, latest.config.get());
        } catch (...) {
            SessionModelReloadResult result;
            result.ok = false;
            result.state = state_snapshot();
            result.error =
                "provider construction plan failed after revalidation";
            return result;
        }
        if (!latest_prepared.has_value()) {
            SessionModelReloadResult result;
            result.ok = false;
            result.state = state_snapshot();
            result.error = "provider construction plan is invalid after revalidation";
            return result;
        }

        if (latest_prepared->fingerprint() != construction->fingerprint) {
            target = std::move(latest);
            continue;
        }
        target = std::move(latest);

        {
            std::lock_guard<std::mutex> state_lock(state_mu_);
            if (selected_generation_ != selected_generation) {
                continue;
            }
            const auto before = applied_revision_.load(std::memory_order_acquire);
            if (target.revision < before) {
                continue;
            }
            selection_changed = state_.name != target.state.name;
            provider_ = std::move(construction->provider);
            state_ = target.state;
            fingerprint_ = construction->fingerprint;
            applied_revision_.store(target.revision, std::memory_order_release);
        }

        SessionModelReloadResult result;
        result.outcome = SessionModelReloadOutcome::Reloaded;
        result.state = target.state;
        result.warning = std::move(warning);
        if (on_transition) {
            try {
                if (!on_transition(
                        result.state,
                        SessionModelTransition{true, selection_changed})) {
                    append_warning(
                        result.warning,
                        "session metadata could not be persisted");
                }
            } catch (...) {
                append_warning(
                    result.warning,
                    "session metadata could not be persisted");
            }
        }
        return result;
    }

    LOG_WARN("[session_model_binding] reload abandoned after repeated stale builds");
    SessionModelReloadResult result;
    result.ok = false;
    result.state = state_snapshot();
    result.error = "model profile changed repeatedly during reload";
    return result;
}

void SessionModelBinding::install_runtime_snapshot(
    std::shared_ptr<LlmProvider> provider,
    SessionModelState state,
    SavedModelsRevision revision) {
    std::lock_guard<std::mutex> operation_lock(operation_mu_);
    std::lock_guard<std::mutex> state_lock(state_mu_);
    ++selected_generation_;
    provider_ = std::move(provider);
    state_ = std::move(state);
    fingerprint_.reset();
    applied_revision_.store(revision, std::memory_order_release);
}

bool SessionModelBinding::synchronize_context_window(
    const std::string& selected_name,
    int context_window,
    const SessionModelTransitionCallback& on_transition) {
    if (selected_name.empty() || context_window <= 0) return false;
    SessionModelState next;
    {
        std::lock_guard<std::mutex> state_lock(state_mu_);
        if (state_.name != selected_name) return false;
        state_.context_window = context_window;
        next = state_;
    }
    if (on_transition) {
        try {
            (void)on_transition(next, SessionModelTransition{});
        } catch (...) {
            // Context synchronization is best-effort and has no public error.
        }
    }
    return true;
}

} // namespace acecode
