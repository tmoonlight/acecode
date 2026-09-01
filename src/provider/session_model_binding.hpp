#pragma once

#include "provider_factory.hpp"
#include "../config/config.hpp"
#include "../config/saved_models_revision.hpp"
#include "../session/session_client.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace acecode {

enum class SessionModelReloadOutcome {
    Reloaded,
    AlreadyCurrent,
    Unresolvable,
};

const char* to_string(SessionModelReloadOutcome outcome) noexcept;

struct SessionModelReloadResult {
    bool ok = true;
    SessionModelReloadOutcome outcome =
        SessionModelReloadOutcome::AlreadyCurrent;
    SessionModelState state;
    std::string warning;
    std::string error;
};

// Adapter-produced snapshot. Profile lookup and config locking stay outside
// the binding; the binding receives an immutable config copy for exact factory
// fallbacks and global derived inputs.
struct SessionModelResolvedTarget {
    SavedModelsRevision revision = 0;
    std::optional<ModelProfile> profile;
    std::shared_ptr<const AppConfig> config;
    SessionModelState state;
};

struct SessionModelTransition {
    bool provider_published = false;
    bool selection_changed = false;
};

using SessionModelRevisionAccessor =
    std::function<SavedModelsRevision()>;
using SessionModelResolver =
    std::function<SessionModelResolvedTarget(const std::string& selected_name)>;
// Return false when best-effort metadata persistence failed. The binding owns
// warning text so adapter exceptions or credential-bearing diagnostics cannot
// escape into logs or HTTP.
using SessionModelTransitionCallback =
    std::function<bool(const SessionModelState&,
                       const SessionModelTransition&)>;

SessionModelState session_model_state_from_profile(
    const AppConfig& config,
    const ModelProfile& profile);

class SessionModelBinding {
public:
    SessionModelBinding() = default;
    SessionModelBinding(const SessionModelBinding&) = delete;
    SessionModelBinding& operator=(const SessionModelBinding&) = delete;

    std::shared_ptr<LlmProvider> provider_snapshot() const;
    SessionModelState state_snapshot() const;
    SavedModelsRevision applied_revision() const noexcept;

    SessionModelReloadResult install_explicit(
        SessionModelResolvedTarget target,
        const SessionModelResolver& resolver,
        const SessionModelTransitionCallback& on_transition = {});

    SessionModelReloadResult ensure_current(
        bool force,
        const SessionModelRevisionAccessor& current_revision,
        const SessionModelResolver& resolver,
        const SessionModelTransitionCallback& on_transition = {});

    // Used for provider snapshots supplied by an embedding host or a focused
    // test double. It still publishes through the binding and deliberately
    // leaves the fingerprint unset, so a later resolvable reload rebuilds it.
    void install_runtime_snapshot(std::shared_ptr<LlmProvider> provider,
                                  SessionModelState state,
                                  SavedModelsRevision revision);

    bool synchronize_context_window(
        const std::string& selected_name,
        int context_window,
        const SessionModelTransitionCallback& on_transition = {});

private:
    SessionModelReloadResult install_target_locked(
        SessionModelResolvedTarget target,
        const std::string& selected_name,
        std::uint64_t selected_generation,
        bool explicit_install,
        const SessionModelResolver& resolver,
        const SessionModelTransitionCallback& on_transition);
    SessionModelReloadResult already_current_result() const;
    SessionModelReloadResult unresolvable_result(
        SavedModelsRevision observed_revision,
        bool record_revision);
    static void append_warning(std::string& warning,
                               const std::string& addition);

    mutable std::mutex state_mu_;
    std::shared_ptr<LlmProvider> provider_;
    SessionModelState state_;
    std::optional<ProviderConstructionFingerprint> fingerprint_;
    std::atomic<SavedModelsRevision> applied_revision_{0};
    std::uint64_t selected_generation_ = 0;

    // Serializes initial install, explicit switch, lazy reload, and forced
    // reload. Provider construction occurs while this mutex is held, but the
    // short state/provider snapshot mutex above remains free for active turns.
    mutable std::mutex operation_mu_;
};

} // namespace acecode
