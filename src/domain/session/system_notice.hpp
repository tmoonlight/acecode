#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace acecode {

// Stable event semantics for clients. Keep content as the text fallback;
// clients translate codes/parameters without interpreting business state.
inline nlohmann::json make_system_notice_metadata(
    const std::string& code,
    nlohmann::json params = nlohmann::json::object(),
    nlohmann::json metadata = nlohmann::json::object()) {
    if (!metadata.is_object()) metadata = nlohmann::json::object();
    metadata["system_notice"] = {
        {"version", 1}, {"code", code},
        {"params", params.is_object() ? std::move(params) : nlohmann::json::object()},
    };
    return metadata;
}

} // namespace acecode
