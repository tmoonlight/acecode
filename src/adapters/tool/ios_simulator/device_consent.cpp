#include "device_consent.hpp"

#include "utils/state_file.hpp"

#include <chrono>
#include <mutex>
#include <set>

namespace acecode::ios_simulator {
namespace {

constexpr const char* kSection = "ios_simulator";
constexpr const char* kKey = "device_consent";

std::mutex g_mu;
std::set<std::string> g_session_grants;  // survives a failed state.json write

nlohmann::json consent_map(const nlohmann::json& state) {
    if (!state.is_object() || !state.contains(kSection) || !state[kSection].is_object()) return nlohmann::json::object();
    const auto& section = state[kSection];
    if (!section.contains(kKey) || !section[kKey].is_object()) return nlohmann::json::object();
    return section[kKey];
}

} // namespace

bool device_consent_granted(const std::string& udid) {
    if (udid.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_session_grants.count(udid)) return true;
    }
    return consent_map(read_state_json()).contains(udid);
}

bool grant_device_consent(const std::string& udid, const std::string& device_name) {
    if (udid.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_session_grants.insert(udid);
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return update_state_json([udid, device_name, now](nlohmann::json& state) {
        if (!state.is_object()) state = nlohmann::json::object();
        auto& section = state[kSection];
        if (!section.is_object()) section = nlohmann::json::object();
        auto& map = section[kKey];
        if (!map.is_object()) map = nlohmann::json::object();
        map[udid] = {{"name", device_name}, {"granted_at", now}};
        return true;
    });
}

bool revoke_device_consent(const std::string& udid) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_session_grants.erase(udid);
    }
    return update_state_json([udid](nlohmann::json& state) {
        if (!state.is_object() || !state.contains(kSection) || !state[kSection].is_object()) return false;
        auto& section = state[kSection];
        if (!section.contains(kKey) || !section[kKey].is_object()) return false;
        return section[kKey].erase(udid) > 0;
    });
}

nlohmann::json list_device_consent() {
    return consent_map(read_state_json());
}

} // namespace acecode::ios_simulator
