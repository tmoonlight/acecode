// src/provider/apply_model_to_session.cpp
#include "apply_model_to_session.hpp"

#include "../config/saved_models_revision.hpp"
#include "../agent_loop.hpp"
#include "../session/session_manager.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

namespace acecode {

ApplyModelResult apply_model_to_session(const ModelProfile& profile,
                                         const ApplyModelDeps& deps) {
    if (!deps.cfg) throw std::runtime_error("config unavailable");
    if (!deps.model_binding) {
        throw std::runtime_error("model binding unavailable");
    }

    SessionModelResolvedTarget target;
    SessionModelResolver resolver = deps.resolver;
    if (resolver) {
        try {
            target = resolver(profile.name);
        } catch (...) {
            throw std::runtime_error("model profile resolution failed");
        }
        if (!target.profile.has_value() || !target.config) {
            throw std::runtime_error("model profile unavailable");
        }
    } else {
        auto config_snapshot = std::make_shared<AppConfig>(*deps.cfg);
        target.revision = current_saved_models_revision();
        target.profile = profile;
        target.config = config_snapshot;
        target.state = session_model_state_from_profile(*config_snapshot, profile);

        // The compatibility/TUI adapter is externally serialized and the
        // caller's explicit profile is authoritative. Daemon callers inject
        // a locked resolver above so they still revalidate against the live
        // saved_models list before publication.
        resolver = [config = deps.cfg, explicit_profile = profile](
                       const std::string& name) {
            auto snapshot = std::make_shared<AppConfig>(*config);
            SessionModelResolvedTarget resolved;
            resolved.revision = current_saved_models_revision();
            resolved.config = snapshot;
            if (name == explicit_profile.name) {
                resolved.profile = explicit_profile;
                resolved.state = session_model_state_from_profile(
                    *snapshot, explicit_profile);
            }
            return resolved;
        };
    }

    SessionModelTransitionCallback transition = deps.on_transition;
    if (!transition) {
        transition = [loop = deps.loop, sm = deps.sm](
                         const SessionModelState& state,
                         const SessionModelTransition& change) {
            if (loop && state.context_window > 0) {
                loop->set_context_window(state.context_window);
            }
            if (!sm || (!change.provider_published &&
                        !change.selection_changed)) {
                return true;
            }
            try {
                return sm->set_active_provider(
                    state.provider, state.model, state.name);
            } catch (...) {
                return false;
            }
        };
    }

    const auto installed = deps.model_binding->install_explicit(
        std::move(target), resolver, transition);
    if (!installed.ok) {
        throw std::runtime_error(
            installed.error.empty() ? "provider create failed" : installed.error);
    }
    return {installed.state, installed.warning};
}

} // namespace acecode
