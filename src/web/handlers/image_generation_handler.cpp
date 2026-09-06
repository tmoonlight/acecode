#include "image_generation_handler.hpp"
#include "../../tool/image_generate/image_generation_policy.hpp"
#include "../../utils/http_url_validation.hpp"

#include <algorithm>

namespace acecode::web {
namespace {
using nlohmann::json;

bool valid_key(const std::string& key) {
    return key.size() <= 16384 && std::none_of(key.begin(), key.end(),
        [](unsigned char ch) { return ch <= 0x20 || ch == 0x7f; });
}

bool valid_quality(const std::string& quality) {
    return quality == "standard" || quality == "high" || quality == "ultra";
}
} // namespace

nlohmann::json image_generation_settings(const AppConfig& config) {
    const auto& ig = config.image_generation;
    auto probe = config;
    probe.image_generation.enabled = true;
    const auto endpoint = image_generation::resolve_endpoint(probe);
    json connections = json::array();
    for (const auto& profile : config.saved_models) {
        if (image_generation::can_reuse_connection(profile)) {
            connections.push_back({{"name", profile.name},
                                   {"has_api_key", !profile.api_key.empty()}});
        }
    }
    return {{"enabled", ig.enabled}, {"source", ig.source},
            {"saved_model_name", ig.saved_model_name}, {"base_url", ig.base_url},
            {"api_key", ig.api_key},
            {"has_api_key", !ig.api_key.empty()},
            {"configured", endpoint.ok},
            {"models", {{"standard", ig.model_standard}, {"high", ig.model_high},
                        {"ultra", ig.model_ultra}}},
            {"default_quality", ig.default_quality}, {"timeout_ms", ig.timeout_ms},
            {"connections", std::move(connections)}};
}

bool apply_image_generation_settings(AppConfig& config,
                                     const nlohmann::json& patch,
                                     std::string& error) {
    error.clear();
    if (!patch.is_object()) {
        error = "expected an image generation settings object";
        return false;
    }
    auto next = config.image_generation;
    const auto text = [&](const json& object, const char* name,
                          std::string& target, std::size_t limit) {
        if (!object.contains(name)) return true;
        if (!object[name].is_string() || object[name].get_ref<const std::string&>().size() > limit) {
            error = std::string("invalid field: ") + name;
            return false;
        }
        target = object[name].get<std::string>();
        return true;
    };
    if (patch.contains("enabled")) {
        if (!patch["enabled"].is_boolean()) { error = "enabled must be boolean"; return false; }
        next.enabled = patch["enabled"].get<bool>();
    }
    if (!text(patch, "source", next.source, 32) ||
        !text(patch, "saved_model_name", next.saved_model_name, 256) ||
        !text(patch, "base_url", next.base_url, 2048) ||
        !text(patch, "api_key", next.api_key, 16384) ||
        !text(patch, "default_quality", next.default_quality, 32)) return false;
    if (next.source != "inline" && next.source != "saved_model") {
        error = "source must be inline or saved_model"; return false;
    }
    if (!next.base_url.empty() && !utils::is_valid_http_base_url(next.base_url)) {
        error = "API URL must be HTTPS (or loopback HTTP), without credentials, query or fragment";
        return false;
    }
    if (!valid_key(next.api_key)) { error = "API key contains invalid characters"; return false; }
    if (!valid_quality(next.default_quality)) { error = "invalid default quality"; return false; }
    if (patch.contains("models")) {
        const auto& models = patch["models"];
        if (!models.is_object()) { error = "models must be an object"; return false; }
        if (!text(models, "standard", next.model_standard, 256) ||
            !text(models, "high", next.model_high, 256) ||
            !text(models, "ultra", next.model_ultra, 256)) return false;
        if (next.model_standard.empty() || next.model_high.empty() || next.model_ultra.empty()) {
            error = "model names must not be empty"; return false;
        }
    }
    if (patch.contains("timeout_ms")) {
        const auto& value = patch["timeout_ms"];
        if (!value.is_number_integer()) { error = "timeout_ms must be an integer"; return false; }
        // Clamp before converting, including JSON integers outside int64 range.
        next.timeout_ms = static_cast<int>(std::clamp(value.get<double>(), 30000.0, 600000.0));
    }
    if (next.source == "saved_model" && !next.saved_model_name.empty()) {
        const auto it = std::find_if(config.saved_models.begin(), config.saved_models.end(),
            [&](const ModelProfile& profile) { return profile.name == next.saved_model_name; });
        const bool disabling_existing_reference = !next.enabled &&
            next.source == config.image_generation.source &&
            next.saved_model_name == config.image_generation.saved_model_name;
        if (!disabling_existing_reference &&
            (it == config.saved_models.end() || !image_generation::can_reuse_connection(*it))) {
            error = "choose an existing OpenAI-compatible connection with a base URL";
            return false;
        }
    }
    config.image_generation = std::move(next);
    return true;
}

} // namespace acecode::web
