#include "image_generation_config.hpp"
#include "utils/http_url_validation.hpp"
#include <stdexcept>

namespace acecode {
nlohmann::json image_generation_config_to_json(const ImageGenerationConfig& config) {
    ImageGenerationConfig defaults;
    nlohmann::json result = nlohmann::json::object();
    if (!config.base_url.empty() && !utils::is_valid_http_base_url(config.base_url)) {
        throw std::runtime_error(
            "image_generation.base_url must be HTTPS (or loopback HTTP) without "
            "credentials, query, or fragment");
    }
    if (config.enabled != defaults.enabled) result["enabled"] = config.enabled;
    if (config.source != defaults.source) result["source"] = config.source;
    if (config.saved_model_name != defaults.saved_model_name) result["saved_model_name"] = config.saved_model_name;
    if (config.base_url != defaults.base_url) result["base_url"] = config.base_url;
    if (config.api_key != defaults.api_key) result["api_key"] = config.api_key;
    nlohmann::json models = nlohmann::json::object();
    if (config.model_standard != defaults.model_standard) models["standard"] = config.model_standard;
    if (config.model_high != defaults.model_high) models["high"] = config.model_high;
    if (config.model_ultra != defaults.model_ultra) models["ultra"] = config.model_ultra;
    if (!models.empty()) result["models"] = std::move(models);
    if (config.default_quality != defaults.default_quality) result["default_quality"] = config.default_quality;
    if (config.timeout_ms != defaults.timeout_ms) result["timeout_ms"] = config.timeout_ms;
    return result;
}
} // namespace acecode
