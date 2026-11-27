#pragma once

#include <cmath>
#include <set>
#include <string>
#include <nlohmann/json.hpp>

namespace acecode::desktop {

inline bool valid_annotation_bounds(const nlohmann::json& bounds, bool region = false) {
    if (!bounds.is_object() || bounds.size() != 4) return false;
    for (const auto& field : {"x", "y", "width", "height"}) {
        if (!bounds.contains(field) || !bounds[field].is_number()) return false;
        const double value = bounds[field].get<double>();
        const bool dimension = std::string(field) == "width" || std::string(field) == "height";
        if (!std::isfinite(value) || std::abs(value) > 10000000 ||
            (dimension && value < 0) || (region && (dimension ? value <= 0 : value < 0))) return false;
    }
    return true;
}

inline bool valid_annotation_targets(const nlohmann::json& targets) {
    if (!targets.is_array() || targets.size() > 64) return false;
    std::set<std::string> identities;
    for (const auto& target : targets) {
        if (!target.is_object()) return false;
        for (const auto& field : {"id", "tag", "text", "locator"}) {
            if (!target.contains(field) || !target[field].is_string() ||
                target[field].get_ref<const std::string&>().size() > 2048) return false;
        }
        if (target["id"] == "" || target["tag"] == "" ||
            !identities.insert(target["id"].get<std::string>()).second ||
            !target.contains("bounds") || !valid_annotation_bounds(target["bounds"])) return false;
    }
    return true;
}

inline nlohmann::json annotation_target_ids(const nlohmann::json& targets) {
    auto identities = nlohmann::json::array();
    for (const auto& target : targets) identities.push_back(target["id"]);
    return identities;
}

} // namespace acecode::desktop
