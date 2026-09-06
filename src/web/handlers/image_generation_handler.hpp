#pragma once

#include "../../config/config.hpp"
#include <nlohmann/json.hpp>

namespace acecode::web {

// Authenticated settings snapshot, including the inline API key for editing.
// Like model settings, callers must not log or cache this response. Reusable
// connections include presence flags only, never their credentials.
nlohmann::json image_generation_settings(const AppConfig& config);

// Apply a partial settings update atomically to a candidate config. An omitted
// api_key preserves the stored secret; an explicit empty string clears it.
bool apply_image_generation_settings(AppConfig& config,
                                     const nlohmann::json& patch,
                                     std::string& error);

} // namespace acecode::web
