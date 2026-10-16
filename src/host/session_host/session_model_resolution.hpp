#pragma once

#include "provider/session_model_binding.hpp"

namespace acecode {
struct SessionRegistryDeps;

namespace session_model_detail {

struct ModelConfigSnapshot {
    // Shared immutable config used by the resolver and provider construction.
    std::shared_ptr<const AppConfig> config;
    SavedModelsRevision revision = 0;
};

ModelConfigSnapshot snapshot_model_config(const SessionRegistryDeps& deps);
std::optional<std::string> apply_session_reasoning_effort(
    ModelProfile& profile, const std::optional<std::string>& effort,
    bool strict = false);
void apply_session_sampling(ModelProfile& profile, const ModelSamplingOptions& sampling);
SessionModelResolvedTarget resolve_target_for_name(
    const SessionRegistryDeps& deps, const std::string& name,
    const std::optional<std::string>& effort = std::nullopt,
    bool strict_effort = false, const ModelSamplingOptions& sampling = {});

} // namespace session_model_detail
} // namespace acecode
