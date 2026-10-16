#include "session_model_resolution.hpp"
#include "session_registry.hpp"
#include "config/saved_models_revision.hpp"

#include <algorithm>
#include <stdexcept>

namespace acecode::session_model_detail {

ModelConfigSnapshot snapshot_model_config(const SessionRegistryDeps& deps) {
    if (!deps.config) return {};
    if (deps.config_mutex) {
        std::shared_lock<std::shared_mutex> lock(*deps.config_mutex);
        return {
            std::make_shared<AppConfig>(*deps.config),
            current_saved_models_revision(),
        };
    }
    return {
        std::make_shared<AppConfig>(*deps.config),
        current_saved_models_revision(),
    };
}

// Only explicitly enabled effort-capable HTTP routes accept a session choice.
bool supports_session_reasoning_effort(const ModelProfile& profile,
                                       const std::string& effort) {
    if ((profile.provider != "openai" && profile.provider != "anthropic") ||
        !profile.reasoning.has_value()) return false;
    const auto& reasoning = *profile.reasoning;
    return reasoning.supported &&
        (reasoning.mandatory || reasoning.enabled.value_or(reasoning.default_enabled)) &&
        !effort.empty() &&
        std::find(reasoning.supported_efforts.begin(),
                  reasoning.supported_efforts.end(), effort) != reasoning.supported_efforts.end();
}

std::optional<std::string> apply_session_reasoning_effort(
    ModelProfile& profile, const std::optional<std::string>& effort,
    bool strict) {
    if (!effort.has_value()) return std::nullopt;
    if (!supports_session_reasoning_effort(profile, *effort)) {
        if (strict) throw SessionReasoningValidationError();
        return std::nullopt;
    }
    profile.reasoning->enabled = true;
    profile.reasoning->effort = *effort;
    profile.reasoning->max_tokens.reset();
    return effort;
}

void apply_session_sampling(ModelProfile& profile, const ModelSamplingOptions& sampling) {
    profile.sampling = merge_model_sampling_options(profile.sampling, sampling);
    std::string error;
    if (!validate_model_sampling_support(profile, error)) {
        throw std::invalid_argument(error);
    }
}

SessionModelResolvedTarget resolve_target_for_name(
    const SessionRegistryDeps& deps,
    const std::string& name,
    const std::optional<std::string>& effort,
    bool strict_effort,
    const ModelSamplingOptions& sampling) {
    auto snapshot = snapshot_model_config(deps);
    SessionModelResolvedTarget target;
    target.revision = snapshot.revision;
    target.config = snapshot.config;
    if (!snapshot.config) return target;
    const auto found = std::find_if(
        snapshot.config->saved_models.begin(),
        snapshot.config->saved_models.end(),
        [&name](const ModelProfile& candidate) {
            return candidate.name == name;
        });
    if (found != snapshot.config->saved_models.end()) {
        target.profile = *found;
        apply_session_sampling(*target.profile, sampling);
        const auto applied = apply_session_reasoning_effort(
            *target.profile, effort, strict_effort);
        target.state = session_model_state_from_profile(*snapshot.config, *target.profile);
        target.state.reasoning_effort = applied;
    }
    return target;
}

} // namespace acecode::session_model_detail
