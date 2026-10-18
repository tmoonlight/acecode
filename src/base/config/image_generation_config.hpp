#pragma once
#include "config.hpp"

namespace acecode {
// Sparse persisted representation, shared with the main config writer.
nlohmann::json image_generation_config_to_json(const ImageGenerationConfig& config);
} // namespace acecode
