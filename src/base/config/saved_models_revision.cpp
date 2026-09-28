#include "saved_models_revision.hpp"

#include <atomic>
#include <utility>

namespace acecode {
namespace {

std::atomic<SavedModelsRevision> g_saved_models_revision{0};

void advance_saved_models_revision() noexcept {
    // live_config is published by the caller before this release operation.
    // Readers pair it with the acquire load below and the config owner's lock.
    g_saved_models_revision.fetch_add(1, std::memory_order_release);
}

} // namespace

SavedModelsRevision current_saved_models_revision() noexcept {
    return g_saved_models_revision.load(std::memory_order_acquire);
}

bool publish_live_config(AppConfig& live_config,
                         const AppConfig& persisted_config,
                         bool publish_saved_models_revision) {
    const bool saved_models_changed = publish_saved_models_revision &&
        !saved_model_lists_equal(live_config.saved_models,
                                 persisted_config.saved_models);
    live_config = persisted_config;
    if (saved_models_changed) advance_saved_models_revision();
    return saved_models_changed;
}

bool publish_live_saved_models(
    AppConfig& live_config,
    std::vector<ModelProfile> saved_models) {
    if (saved_model_lists_equal(live_config.saved_models, saved_models)) {
        return false;
    }
    live_config.saved_models = std::move(saved_models);
    advance_saved_models_revision();
    return true;
}

} // namespace acecode
