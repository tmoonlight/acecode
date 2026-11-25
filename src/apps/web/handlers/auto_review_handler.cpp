#include "auto_review_handler.hpp"

#include <algorithm>

namespace acecode::web {

nlohmann::json auto_review_settings(const AppConfig& config) {
    const std::string& name = config.auto_review.model_name;
    nlohmann::json models = nlohmann::json::array();
    bool configured = false;
    for (const auto& profile : config.saved_models) {
        models.push_back({{"name", profile.name}, {"provider", profile.provider},
                          {"model", profile.model}});
        if (!name.empty() && profile.name == name) configured = true;
    }
    return {{"model_name", name}, {"configured", configured}, {"models", std::move(models)}};
}

bool apply_auto_review_settings(AppConfig& config, const nlohmann::json& patch, std::string& error) {
    error.clear();
    if (!patch.is_object()) {
        error = "expected an auto review settings object";
        return false;
    }
    std::string next = config.auto_review.model_name;
    for (const auto& item : patch.items()) {
        if (item.key() == "model_name" && item.value().is_string()) {
            next = item.value().get<std::string>();
        } else {
            error = "expected only model_name (string)";
            return false;
        }
    }
    const bool exists = !next.empty() && std::any_of(
        config.saved_models.begin(), config.saved_models.end(),
        [&](const ModelProfile& profile) { return profile.name == next; });
    if (!next.empty() && !exists && next != config.auto_review.model_name) {
        error = "choose an existing reviewer model";
        return false;
    }
    config.auto_review.model_name = std::move(next);
    return true;
}

} // namespace acecode::web
