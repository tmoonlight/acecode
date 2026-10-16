#include "config.hpp"

#include <algorithm>
#include <stdexcept>

namespace acecode {

void load_ios_simulator_config_json(const nlohmann::json& j, IosSimulatorConfig& out) {
    if (!j.is_object()) throw std::runtime_error("ios_simulator must be an object");
    if (j.contains("enabled")) {
        if (!j["enabled"].is_boolean()) throw std::runtime_error("ios_simulator.enabled must be a boolean");
        out.enabled = j["enabled"].get<bool>();
    }
    if (j.contains("xcode_path")) {
        if (!j["xcode_path"].is_string()) throw std::runtime_error("ios_simulator.xcode_path must be a string");
        out.xcode_path = j["xcode_path"].get<std::string>();
    }
    // Stream tuning is clamped silently: out-of-range values are not worth a failed start.
    if (j.contains("stream_fps") && j["stream_fps"].is_number())
        out.stream_fps = std::clamp(j["stream_fps"].get<int>(), 5, 60);
    if (j.contains("stream_scale") && j["stream_scale"].is_number())
        out.stream_scale = std::clamp(j["stream_scale"].get<double>(), 0.25, 1.0);
    if (j.contains("stream_quality") && j["stream_quality"].is_number())
        out.stream_quality = std::clamp(j["stream_quality"].get<double>(), 0.3, 0.95);
}

nlohmann::json ios_simulator_config_to_json(const IosSimulatorConfig& cfg) {
    const IosSimulatorConfig defaults;
    nlohmann::json out = nlohmann::json::object();
    if (cfg.enabled != defaults.enabled) out["enabled"] = cfg.enabled;
    if (!cfg.xcode_path.empty()) out["xcode_path"] = cfg.xcode_path;
    if (cfg.stream_fps != defaults.stream_fps) out["stream_fps"] = cfg.stream_fps;
    if (cfg.stream_scale != defaults.stream_scale) out["stream_scale"] = cfg.stream_scale;
    if (cfg.stream_quality != defaults.stream_quality) out["stream_quality"] = cfg.stream_quality;
    return out;
}

} // namespace acecode
