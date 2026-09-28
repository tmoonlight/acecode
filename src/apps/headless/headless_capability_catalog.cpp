#include "headless_capability_catalog.hpp"

#include <cctype>
#include <map>
#include <sstream>

namespace acecode::headless {

namespace {

std::string collapse_ascii_whitespace(const std::string& value) {
    std::string result;
    bool pending_space = false;
    for (const unsigned char c : value) {
        if (std::isspace(c) != 0) {
            pending_space = !result.empty();
            continue;
        }
        if (pending_space) {
            result.push_back(' ');
            pending_space = false;
        }
        result.push_back(static_cast<char>(c));
    }
    return result;
}

} // namespace

std::string format_capability_catalog(
    const std::string& heading,
    const std::vector<CapabilityCatalogEntry>& entries) {
    std::map<std::string, std::string> unique_entries;
    for (const auto& entry : entries) {
        if (entry.name.empty()) continue;
        unique_entries.emplace(
            entry.name, collapse_ascii_whitespace(entry.description));
    }

    std::ostringstream out;
    out << heading << " (" << unique_entries.size() << "):\n";
    if (unique_entries.empty()) {
        out << "  (none)\n";
        return out.str();
    }

    for (const auto& [name, description] : unique_entries) {
        out << "  " << name << "\n";
        if (!description.empty()) {
            out << "    " << description << "\n";
        }
    }
    return out.str();
}

} // namespace acecode::headless
