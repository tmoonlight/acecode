#include "config.hpp"

#include <algorithm>

namespace acecode {

namespace {

int clamp_int(int value, int lo, int hi) {
    return std::max(lo, std::min(hi, value));
}

} // namespace

void load_memory_config_json(const nlohmann::json& j, MemoryConfig& out) {
    if (!j.is_object()) return;
    if (j.contains("enabled") && j["enabled"].is_boolean()) out.enabled = j["enabled"].get<bool>();
    if (j.contains("max_index_bytes") && j["max_index_bytes"].is_number_integer()) {
        const long long v = j["max_index_bytes"].get<long long>();
        if (v > 0) out.max_index_bytes = static_cast<std::size_t>(v);
    }
    if (!j.contains("summary") || !j["summary"].is_object()) return;
    const auto& sj = j["summary"];
    auto& summary = out.summary;
    if (sj.contains("enabled") && sj["enabled"].is_boolean()) summary.enabled = sj["enabled"].get<bool>();
    if (sj.contains("model_name") && sj["model_name"].is_string()) {
        summary.model_name = sj["model_name"].get<std::string>();
    }
    if (sj.contains("idle_minutes") && sj["idle_minutes"].is_number_integer()) {
        summary.idle_minutes = clamp_int(sj["idle_minutes"].get<int>(), 5, 1440);
    }
    if (sj.contains("max_session_age_days") && sj["max_session_age_days"].is_number_integer()) {
        summary.max_session_age_days = clamp_int(sj["max_session_age_days"].get<int>(), 1, 90);
    }
}

nlohmann::json memory_config_to_json(const MemoryConfig& cfg) {
    const MemoryConfig d;
    nlohmann::json out = nlohmann::json::object();
    if (cfg.enabled != d.enabled) out["enabled"] = cfg.enabled;
    if (cfg.max_index_bytes != d.max_index_bytes) out["max_index_bytes"] = cfg.max_index_bytes;
    nlohmann::json summary = nlohmann::json::object();
    if (cfg.summary.enabled != d.summary.enabled) summary["enabled"] = cfg.summary.enabled;
    if (cfg.summary.model_name != d.summary.model_name) summary["model_name"] = cfg.summary.model_name;
    if (cfg.summary.idle_minutes != d.summary.idle_minutes) {
        summary["idle_minutes"] = cfg.summary.idle_minutes;
    }
    if (cfg.summary.max_session_age_days != d.summary.max_session_age_days) {
        summary["max_session_age_days"] = cfg.summary.max_session_age_days;
    }
    if (!summary.empty()) out["summary"] = std::move(summary);
    return out;
}

} // namespace acecode
