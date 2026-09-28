#pragma once

#include "config.hpp"

#include <cstdint>
#include <vector>

namespace acecode {

using SavedModelsRevision = std::uint64_t;

// O(1) process-wide read used by the ordinary input fast path.
SavedModelsRevision current_saved_models_revision() noexcept;

// Publish a persisted AppConfig into the live process. The revision advances
// exactly once when the saved-model list changes structurally, and only after
// the new list is visible in live_config.
bool publish_live_config(AppConfig& live_config,
                         const AppConfig& persisted_config,
                         bool publish_saved_models_revision);

// Connector-specific publication: only the saved-model list is replaced.
// Returns true exactly when a structural change was published.
bool publish_live_saved_models(
    AppConfig& live_config,
    std::vector<ModelProfile> saved_models);

} // namespace acecode
