#pragma once

#include "../../provider/auth/xai_auth.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace acecode::web {

// Pure response builders for the managed Grok Coding Plan routes. These
// serializers deliberately expose device-flow fields only; OAuth tokens and
// persisted identity never cross the daemon API boundary.
nlohmann::json grok_auth_status_to_json(bool authenticated);
nlohmann::json grok_device_code_to_json(const GrokDeviceCodeResponse& response,
                                        std::int64_t now_unix_ms);
nlohmann::json grok_device_poll_to_json(const GrokDevicePollResult& result,
                                        bool authenticated);

std::optional<std::string> parse_grok_device_poll_request(
    const nlohmann::json& body,
    std::string& error);

} // namespace acecode::web
