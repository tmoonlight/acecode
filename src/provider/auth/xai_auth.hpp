#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace acecode {

// Grok Build protocol constants mirrored from chenyme/grok2api. These are
// public OAuth client/protocol identifiers, not user secrets.
namespace grok_build {
inline constexpr char kOAuthClientId[] = "b1a00492-073a-47ea-816f-4c329264a828";
inline constexpr char kOAuthScope[] =
    "openid profile email offline_access grok-cli:access api:access "
    "conversations:read conversations:write workspaces:read workspaces:write";
inline constexpr char kDeviceUrl[] = "https://auth.x.ai/oauth2/device/code";
inline constexpr char kTokenUrl[] = "https://auth.x.ai/oauth2/token";
inline constexpr char kBuildBaseUrl[] = "https://cli-chat-proxy.grok.com/v1";
inline constexpr char kClientVersion[] = "0.2.119";
inline constexpr char kClientIdentifier[] = "grok-shell";
inline constexpr char kTokenAuth[] = "xai-grok-cli";
} // namespace grok_build

struct GrokAuthConfig {
    std::string client_id = grok_build::kOAuthClientId;
    std::string scope = grok_build::kOAuthScope;
    std::string device_url = grok_build::kDeviceUrl;
    std::string token_url = grok_build::kTokenUrl;
    std::string build_base_url = grok_build::kBuildBaseUrl;
    std::string client_version = grok_build::kClientVersion;
    // Test/integration override. Empty uses ~/.acecode/grok_auth.json.
    std::string credential_path;
    int timeout_ms = 30000;
};

struct GrokDeviceCodeResponse {
    std::string device_code;
    std::string user_code;
    std::string verification_uri;
    std::string verification_uri_complete;
    int interval = 5;
    int expires_in = 1800;
    int status_code = 0;
    std::string error;
    std::string message;
};

struct GrokAuthTokens {
    std::string access_token;
    std::string refresh_token;
    int64_t expires_at = 0;
    std::string user_id;
    std::string email;
};

struct GrokDevicePollResult {
    // "pending", "slow_down", "authorized", "expired", or "failed".
    std::string status;
    GrokAuthTokens tokens;
    int status_code = 0;
    std::string error;
    std::string message;
    int interval_delta_seconds = 0;
};

struct GrokAccessTokenResult {
    bool ok = false;
    GrokAuthTokens tokens;
    int status_code = 0;
    std::string error;
    std::string message;
};

struct GrokModelsResult {
    std::vector<std::string> models;
    int status_code = 0;
    std::string error;
    std::string message;
};

// Pure parsers are public so protocol edge cases can be covered without live
// OAuth credentials or network access.
GrokDeviceCodeResponse parse_grok_device_code_response(
    int status_code,
    const std::string& response_body);
GrokDevicePollResult parse_grok_token_response(
    int status_code,
    const std::string& response_body,
    const std::string& fallback_refresh_token = "",
    bool device_flow = true);
std::vector<std::string> parse_grok_model_ids(
    const std::string& response_body,
    std::string* error = nullptr);

// Redacts OAuth credentials from JSON, form data, headers, and diagnostics.
std::string redact_grok_auth_diagnostic(const std::string& text);

GrokDeviceCodeResponse request_grok_device_code(
    const GrokAuthConfig& config = {});
GrokDevicePollResult poll_grok_device_code_once(
    const std::string& device_code,
    const GrokAuthConfig& config = {});

// Returns a token that is valid for at least 60 seconds. Refresh is serialized
// process-wide and persisted atomically. On a 401, pass the rejected token with
// force_refresh=true; a token already rotated by another thread is reused.
GrokAccessTokenResult ensure_grok_access_token(
    bool force_refresh = false,
    const std::string& rejected_access_token = "",
    const GrokAuthConfig& config = {});

GrokModelsResult fetch_grok_model_ids(const GrokAuthConfig& config = {});

std::string grok_auth_file_path();
GrokAuthTokens load_grok_auth_tokens(const std::string& path = "");
bool save_grok_auth_tokens(
    const GrokAuthTokens& tokens,
    const std::string& path = "",
    std::string* error = nullptr);
bool has_saved_grok_auth(const std::string& path = "");
bool delete_grok_auth(const std::string& path = "", std::string* error = nullptr);

} // namespace acecode
