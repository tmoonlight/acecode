#include "summary_generation_handler.hpp"

#include <algorithm>

namespace acecode::web {

nlohmann::json summary_generation_settings(const AppConfig& config) {
    const auto& summary = config.summary_generation;
    nlohmann::json models = nlohmann::json::array();
    bool configured = false;
    for (const auto& profile : config.saved_models) {
        models.push_back({{"name", profile.name}, {"provider", profile.provider},
                          {"model", profile.model}});
        if (!summary.model_name.empty() && profile.name == summary.model_name) {
            configured = true;
        }
    }
    return {{"enabled", summary.enabled}, {"model_name", summary.model_name},
            {"configured", configured}, {"models", std::move(models)}};
}

bool apply_summary_generation_settings(AppConfig& config,
                                       const nlohmann::json& patch,
                                       std::string& error) {
    error.clear();
    if (!patch.is_object()) {
        error = "expected a summary generation settings object";
        return false;
    }
    auto next = config.summary_generation;
    for (const auto& item : patch.items()) {
        if (item.key() == "enabled" && item.value().is_boolean()) {
            next.enabled = item.value().get<bool>();
        } else if (item.key() == "model_name" && item.value().is_string()) {
            next.model_name = item.value().get<std::string>();
        } else {
            error = "expected only enabled (boolean) and model_name (string)";
            return false;
        }
    }
    const bool exists = !next.model_name.empty() && std::any_of(
        config.saved_models.begin(), config.saved_models.end(),
        [&](const ModelProfile& profile) { return profile.name == next.model_name; });
    // A removed reference can still be disabled or cleared. A newly selected
    // reference, or any enabled override, must point to a saved model.
    if (!exists && (next.enabled ||
        (!next.model_name.empty() && next.model_name != config.summary_generation.model_name))) {
        error = "choose an existing summary model";
        return false;
    }
    config.summary_generation = std::move(next);
    return true;
}

} // namespace acecode::web
