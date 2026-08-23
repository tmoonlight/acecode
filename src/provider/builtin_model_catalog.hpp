#pragma once

#include "../utils/models_dev_catalog.hpp"

#include <string>

namespace acecode {

// Canonical first-party ACEModel catalog entry shared by every UI surface.
// The returned object has process lifetime so callers may safely retain its
// address while running a synchronous picker or serializer.
const ProviderEntry& acemodel_catalog_provider();

// Case-insensitive identity check for deduplicating external catalog entries.
bool is_acemodel_provider_id(const std::string& provider_id);

} // namespace acecode
