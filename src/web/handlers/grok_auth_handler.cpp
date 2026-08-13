#include "grok_auth_handler.hpp"

namespace acecode::web {

nlohmann::json grok_auth_status_to_json(bool authenticated) {
    return {
        {"provider", "grok"},
        {"authenticated", authenticated},
    };
}

nlohmann::json grok_device_code_to_json(const GrokDeviceCodeResponse& response,
                                        std::int64_t now_unix_ms) {
    return {
        {"status", "pending"},
        {"provider", "grok"},
        {"authenticated", false},
        {"device_code", response.device_code},
        {"user_code", response.user_code},
        {"verification_uri", response.verification_uri},
        {"verification_uri_complete", response.verification_uri_complete},
        {"interval", response.interval},
        {"expires_in", response.expires_in},
        {"expires_at_unix_ms",
         now_unix_ms + static_cast<std::int64_t>(response.expires_in) * 1000},
    };
}

nlohmann::json grok_device_poll_to_json(const GrokDevicePollResult& result,
                                        bool authenticated) {
    nlohmann::json response{
        {"status", result.status == "authorized" ? "authenticated" : result.status},
        {"provider", "grok"},
        {"authenticated", authenticated},
        {"interval_delta_seconds", result.interval_delta_seconds},
    };
    if (!result.error.empty()) {
        response["error"] = redact_grok_auth_diagnostic(result.error);
    }
    if (!result.message.empty()) {
        response["message"] = redact_grok_auth_diagnostic(result.message);
    }
    return response;
}

std::optional<std::string> parse_grok_device_poll_request(
        const nlohmann::json& body,
        std::string& error) {
    if (!body.is_object() || !body.contains("device_code") ||
        !body["device_code"].is_string()) {
        error = "expected {device_code: string}";
        return std::nullopt;
    }
    const std::string device_code = body["device_code"].get<std::string>();
    if (device_code.empty()) {
        error = "device_code must not be empty";
        return std::nullopt;
    }
    error.clear();
    return device_code;
}

} // namespace acecode::web
