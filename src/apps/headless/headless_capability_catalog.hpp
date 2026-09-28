#pragma once

#include <string>
#include <vector>

namespace acecode::headless {

struct CapabilityCatalogEntry {
    std::string name;
    std::string description;
};

// Format a deterministic human-readable catalog. Names are sorted and
// deduplicated; descriptions are collapsed to one line. The returned string
// always ends with a newline.
std::string format_capability_catalog(
    const std::string& heading,
    const std::vector<CapabilityCatalogEntry>& entries);

} // namespace acecode::headless
