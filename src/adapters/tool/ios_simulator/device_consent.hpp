#pragma once

// Per-simulator consent: the first time the agent wants to control a device
// the user is asked once; the answer is remembered in state.json
// (ios_simulator.device_consent.<udid>) and can be revoked in Settings.

#include <nlohmann/json.hpp>

#include <string>

namespace acecode::ios_simulator {

bool device_consent_granted(const std::string& udid);
// Returns false when state.json could not be written (the grant still holds
// for this process).
bool grant_device_consent(const std::string& udid, const std::string& device_name);
bool revoke_device_consent(const std::string& udid);
// {udid: {name, granted_at}}
nlohmann::json list_device_consent();

} // namespace acecode::ios_simulator
