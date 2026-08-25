#pragma once

#include "../utils/models_dev_catalog.hpp"

#include <string>

namespace acecode {

// Canonical first-party ACEModel catalog entry shared by every UI surface.
// Model context values are local fallbacks; valid `/models` metadata wins.
// The returned object has process lifetime so callers may safely retain its
// address while running a synchronous picker or serializer.
const ProviderEntry& acemodel_catalog_provider();

// Case-insensitive identity check for deduplicating external catalog entries.
bool is_acemodel_provider_id(const std::string& provider_id);

// Case-insensitive lookup against the canonical ACEModel model list. The
// returned pointer has process lifetime.
const ModelEntry* find_acemodel_catalog_model(const std::string& model_id);

// Matches the fixed first-party endpoint while tolerating surrounding
// whitespace, URL case, and trailing slashes.
bool is_acemodel_base_url(const std::string& base_url);

} // namespace acecode
