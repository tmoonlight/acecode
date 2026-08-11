#pragma once

#include "models_dev_catalog.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode {

struct RecommendedReasoning {
    bool supported = false;
    bool mandatory = false;
    bool default_enabled = false;
    std::vector<std::string> supported_efforts;
    std::optional<std::string> default_effort;
    bool supports_max_tokens = false;
};

struct RecommendedModel {
    std::string provider_id;
    std::string model_id;
    std::string name;
    int context_window = 0;
    int max_output_tokens = 0;
    std::vector<std::string> capabilities;
    RecommendedReasoning reasoning;
    bool deprecated = false;
    std::string privacy_warning;
};

struct RecommendedModelsManifest {
    int schema_version = 0;
    std::string selected_at;
    std::string source_url;
    std::string provider_id;
    std::vector<RecommendedModel> models;
};

// Strictly parse the reviewed recommendation manifest. Required fields and the
// fixed five-member contract are rejected on mismatch instead of defaulting.
std::optional<RecommendedModelsManifest> parse_recommended_models_manifest(
    const nlohmann::json& value,
    std::string& error);

// Read and parse <models_dev_dir>/recommended_models.json.
std::optional<RecommendedModelsManifest> load_recommended_models_manifest(
    const std::filesystem::path& models_dev_dir,
    std::string& error);

// Apply current catalog metadata without adding, removing, replacing, or
// reordering reviewed members. Missing catalog entries keep offline fallback
// metadata from the manifest.
RecommendedModelsManifest supplement_recommended_models(
    RecommendedModelsManifest manifest,
    const std::vector<ProviderEntry>& catalog);

nlohmann::json recommended_reasoning_to_json(
    const RecommendedReasoning& reasoning);
nlohmann::json recommended_model_to_json(const RecommendedModel& model);

} // namespace acecode
