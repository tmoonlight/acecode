#include "config.hpp"

#include <algorithm>

namespace acecode {

namespace {

int clamp_int(int value, int lo, int hi) {
    return std::max(lo, std::min(hi, value));
}

void read_int(const nlohmann::json& j, const char* key, int& out) {
    if (j.contains(key) && j[key].is_number_integer()) out = j[key].get<int>();
}

} // namespace

void load_swarm_config_json(const nlohmann::json& j, SwarmConfig& out) {
    if (!j.is_object() || !j.contains("mesh") || !j["mesh"].is_object()) return;
    const auto& mj = j["mesh"];
    auto& mesh = out.mesh;
    read_int(mj, "max_concurrent_agents", mesh.max_concurrent_agents);
    read_int(mj, "min_wait_timeout_ms", mesh.min_wait_timeout_ms);
    read_int(mj, "default_wait_timeout_ms", mesh.default_wait_timeout_ms);
    read_int(mj, "max_wait_timeout_ms", mesh.max_wait_timeout_ms);
    if (mj.contains("expose_model_overrides") && mj["expose_model_overrides"].is_boolean()) {
        mesh.expose_model_overrides = mj["expose_model_overrides"].get<bool>();
    }
    // 越界静默夹取:这些只是调优值,不值得阻塞启动。
    mesh.max_concurrent_agents = clamp_int(mesh.max_concurrent_agents, 2, 64);
    mesh.min_wait_timeout_ms = clamp_int(mesh.min_wait_timeout_ms, 1000, 600000);
    mesh.max_wait_timeout_ms =
        clamp_int(mesh.max_wait_timeout_ms, mesh.min_wait_timeout_ms, 86400000);
    mesh.default_wait_timeout_ms = clamp_int(mesh.default_wait_timeout_ms,
        mesh.min_wait_timeout_ms, mesh.max_wait_timeout_ms);
}

nlohmann::json swarm_config_to_json(const SwarmConfig& cfg) {
    const MeshSwarmConfig defaults;
    nlohmann::json mesh = nlohmann::json::object();
    if (cfg.mesh.max_concurrent_agents != defaults.max_concurrent_agents)
        mesh["max_concurrent_agents"] = cfg.mesh.max_concurrent_agents;
    if (cfg.mesh.min_wait_timeout_ms != defaults.min_wait_timeout_ms)
        mesh["min_wait_timeout_ms"] = cfg.mesh.min_wait_timeout_ms;
    if (cfg.mesh.default_wait_timeout_ms != defaults.default_wait_timeout_ms)
        mesh["default_wait_timeout_ms"] = cfg.mesh.default_wait_timeout_ms;
    if (cfg.mesh.max_wait_timeout_ms != defaults.max_wait_timeout_ms)
        mesh["max_wait_timeout_ms"] = cfg.mesh.max_wait_timeout_ms;
    if (cfg.mesh.expose_model_overrides != defaults.expose_model_overrides)
        mesh["expose_model_overrides"] = cfg.mesh.expose_model_overrides;
    nlohmann::json out = nlohmann::json::object();
    if (!mesh.empty()) out["mesh"] = std::move(mesh);
    return out;
}

} // namespace acecode
