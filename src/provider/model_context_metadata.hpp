#pragma once

#include <nlohmann/json_fwd.hpp>

namespace acecode {

// Reads a positive token-capacity value from common OpenAI-compatible model
// metadata shapes. Returns 0 when no supported field contains a valid value.
int model_context_window_from_metadata(const nlohmann::json& value);

} // namespace acecode
