#pragma once

#include "../../provider/models_dev_registry.hpp"
#include "../../utils/models_dev_catalog.hpp"

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode::web {

constexpr int kDefaultModelCatalogQueryLimit = 50;
constexpr int kMaxModelCatalogQueryLimit = 100;

// Pure serializers used by authenticated routes and focused unit tests.
nlohmann::json model_catalog_summary_to_json(
    const std::vector<ProviderEntry>& providers,
    const RegistrySource& source,
    unsigned long long version);

// Returns nullopt when provider_id is unknown. Search is case-insensitive over
// model ID/name, exact IDs sort first, and results are capped at 100.
std::optional<nlohmann::json> query_model_catalog_to_json(
    const std::vector<ProviderEntry>& providers,
    const std::string& provider_id,
    const std::string& query,
    int limit);

nlohmann::json catalog_metadata_to_json(const RegistrySource& source,
                                        unsigned long long version);

} // namespace acecode::web
