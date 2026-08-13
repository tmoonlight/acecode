#include "xai_auth.hpp"

#include "../../config/config.hpp"
#include "../../network/proxy_resolver.hpp"
#include "../../utils/atomic_file.hpp"
#include "../../utils/utf8_path.hpp"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode {
namespace {

std::mutex g_grok_refresh_mutex;

int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string auth_path_or_default(const std::string& path) {
    return path.empty() ? grok_auth_file_path() : path;
}

std::string grok_user_agent(const GrokAuthConfig& config) {
    return "grok-shell/" + config.client_version + " (linux; x86_64)";
}

std::string oauth_message(const nlohmann::json& value) {
    std::vector<std::string> parts;
    for (const char* key : {"error_description", "message"}) {
        if (value.contains(key) && value[key].is_string()) {
            const std::string part = value[key].get<std::string>();
            if (!part.empty() &&
                std::find(parts.begin(), parts.end(), part) == parts.end()) {
                parts.push_back(part);
            }
        }
    }
    std::ostringstream out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out << " - ";
        out << parts[i];
    }
    return redact_grok_auth_diagnostic(out.str());
}

std::string base64url_decode(std::string value) {
    std::replace(value.begin(), value.end(), '-', '+');
    std::replace(value.begin(), value.end(), '_', '/');
    while (value.size() % 4 != 0) value.push_back('=');

    static constexpr unsigned char kInvalid = 0xff;
    static const unsigned char table[256] = {
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,62,0xff,0xff,0xff,63,
        52,53,54,55,56,57,58,59,60,61,0xff,0xff,0xff,0,0xff,0xff,
        0xff,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,0xff,0xff,0xff,0xff,0xff,
        0xff,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    };

    std::string out;
    unsigned int accumulator = 0;
    int bits = -8;
    for (unsigned char ch : value) {
        if (ch == '=') break;
        const unsigned char decoded = table[ch];
        if (decoded == kInvalid) return "";
        accumulator = (accumulator << 6) | decoded;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((accumulator >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}

void fill_identity_from_id_token(const std::string& id_token,
                                 GrokAuthTokens& tokens) {
    const std::size_t first = id_token.find('.');
    if (first == std::string::npos) return;
    const std::size_t second = id_token.find('.', first + 1);
    if (second == std::string::npos) return;
    try {
        const auto payload = nlohmann::json::parse(
            base64url_decode(id_token.substr(first + 1, second - first - 1)));
        tokens.user_id = payload.value("sub", "");
        tokens.email = payload.value("email", "");
    } catch (...) {
        // Identity headers are optional. A valid OAuth token remains usable.
    }
}

cpr::Header oauth_device_headers(const GrokAuthConfig& config) {
    return {
        {"Accept", "application/json"},
        {"x-grok-client-version", config.client_version},
        {"x-grok-client-surface", "ui"},
    };
}

cpr::Header model_headers(const GrokAuthConfig& config,
                          const GrokAuthTokens& tokens) {
    cpr::Header headers{
        {"Accept", "application/json"},
        {"Authorization", "Bearer " + tokens.access_token},
        {"X-XAI-Token-Auth", grok_build::kTokenAuth},
        {"x-grok-client-version", config.client_version},
        {"x-grok-client-identifier", grok_build::kClientIdentifier},
        {"x-grok-client-mode", "headless"},
        {"User-Agent", grok_user_agent(config)},
    };
    if (!tokens.user_id.empty()) headers["x-userid"] = tokens.user_id;
    if (!tokens.email.empty()) headers["x-email"] = tokens.email;
    return headers;
}

GrokDevicePollResult refresh_grok_tokens(const GrokAuthTokens& current,
                                         const GrokAuthConfig& config) {
    GrokDevicePollResult result;
    if (current.refresh_token.empty()) {
        result.status = "failed";
        result.status_code = 401;
        result.error = "GROK_REFRESH_TOKEN_MISSING";
        result.message = "Grok refresh token is missing";
        return result;
    }

    auto proxy = network::proxy_options_for(config.token_url);
    const cpr::Response response = cpr::Post(
        cpr::Url{config.token_url},
        cpr::Header{{"Accept", "application/json"}},
        cpr::Payload{
            {"grant_type", "refresh_token"},
            {"client_id", config.client_id},
            {"refresh_token", current.refresh_token},
        },
        network::build_ssl_options(proxy),
        proxy.proxies,
        proxy.auth,
        cpr::Timeout{config.timeout_ms});

    result = parse_grok_token_response(
        static_cast<int>(response.status_code), response.text,
        current.refresh_token, false);
    if (response.status_code == 0) {
        result.status = "failed";
        result.error = "GROK_AUTH_UNREACHABLE";
        result.message = redact_grok_auth_diagnostic(response.error.message);
    }
    if (result.status == "authorized") {
        if (result.tokens.user_id.empty()) result.tokens.user_id = current.user_id;
        if (result.tokens.email.empty()) result.tokens.email = current.email;
    }
    return result;
}

GrokModelsResult fetch_grok_model_ids_once(const GrokAuthConfig& config,
                                           const GrokAuthTokens& tokens) {
    GrokModelsResult result;
    std::string base = config.build_base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    const std::string url = base + "/models";
    auto proxy = network::proxy_options_for(url);
    const cpr::Response response = cpr::Get(
        cpr::Url{url},
        model_headers(config, tokens),
        network::build_ssl_options(proxy),
        proxy.proxies,
        proxy.auth,
        cpr::Timeout{config.timeout_ms});
    result.status_code = static_cast<int>(response.status_code);
    if (response.status_code == 0) {
        result.error = "GROK_MODELS_UNREACHABLE";
        result.message = redact_grok_auth_diagnostic(response.error.message);
        return result;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        result.error = response.status_code == 401
            ? "GROK_AUTH_EXPIRED"
            : "GROK_MODELS_HTTP_ERROR";
        result.message = "Grok Build returned HTTP " +
            std::to_string(response.status_code);
        return result;
    }
    result.models = parse_grok_model_ids(response.text, &result.message);
    if (!result.message.empty()) result.error = "GROK_MODELS_BAD_JSON";
    return result;
}

} // namespace

std::string redact_grok_auth_diagnostic(const std::string& text) {
    std::string redacted = text;
    const std::regex json_secret(
        R"REGEX(("[^"]*(?:token|authorization|cookie|secret|password|credential|assertion|code[_-]?verifier|device[_-]?code|email|user[_-]?id|userid)[^"]*"\s*:\s*)"[^"]*")REGEX",
        std::regex::icase);
    redacted = std::regex_replace(redacted, json_secret, "$1\"[REDACTED]\"");
    const std::regex pair_secret(
        R"(((?:[A-Za-z0-9_.-]*(?:token|authorization|cookie|secret|password|credential|assertion|code[_-]?verifier|device[_-]?code|email|user[_-]?id|userid)[A-Za-z0-9_.-]*)=)[^&\s"'<>]+)",
        std::regex::icase);
    redacted = std::regex_replace(redacted, pair_secret, "$1[REDACTED]");
    const std::regex bearer(R"((Bearer\s+)[A-Za-z0-9._~+\-/]+=*)",
                            std::regex::icase);
    redacted = std::regex_replace(redacted, bearer, "$1[REDACTED]");
    const std::regex jwt(
        R"(\b[A-Za-z0-9_-]{2,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{2,}\b)");
    redacted = std::regex_replace(redacted, jwt, "[REDACTED]");
    const std::regex opaque_secret(R"(\b[A-Za-z0-9_~+\-/=]{80,}\b)");
    return std::regex_replace(redacted, opaque_secret, "[REDACTED]");
}

GrokDeviceCodeResponse parse_grok_device_code_response(
        int status_code,
        const std::string& response_body) {
    GrokDeviceCodeResponse result;
    result.status_code = status_code;
    try {
        const auto value = nlohmann::json::parse(response_body);
        if (status_code < 200 || status_code >= 300) {
            result.error = value.value("error", "GROK_DEVICE_AUTH_FAILED");
            result.message = oauth_message(value);
            if (result.message.empty()) result.message = "Grok device authorization failed";
            return result;
        }
        result.device_code = value.value("device_code", "");
        result.user_code = value.value("user_code", "");
        result.verification_uri = value.value("verification_uri", "");
        result.verification_uri_complete =
            value.value("verification_uri_complete", "");
        result.interval = value.value("interval", 5);
        result.expires_in = value.value("expires_in", 1800);
        if (result.interval <= 0) result.interval = 5;
        if (result.expires_in <= 0) result.expires_in = 1800;
        if (result.device_code.empty() || result.user_code.empty() ||
            result.verification_uri.empty()) {
            result.error = "GROK_DEVICE_AUTH_BAD_RESPONSE";
            result.message = "Grok device authorization response is incomplete";
        }
    } catch (const std::exception& e) {
        result.error = "GROK_DEVICE_AUTH_BAD_JSON";
        result.message = e.what();
    }
    result.message = redact_grok_auth_diagnostic(result.message);
    return result;
}

GrokDevicePollResult parse_grok_token_response(
        int status_code,
        const std::string& response_body,
        const std::string& fallback_refresh_token,
        bool device_flow) {
    GrokDevicePollResult result;
    result.status_code = status_code;
    try {
        const auto value = nlohmann::json::parse(response_body);
        const std::string access = value.value("access_token", "");
        if (status_code >= 200 && status_code < 300 && !access.empty()) {
            result.status = "authorized";
            result.tokens.access_token = access;
            result.tokens.refresh_token =
                value.value("refresh_token", fallback_refresh_token);
            int64_t expires_in = value.value("expires_in", int64_t{3600});
            if (expires_in <= 0) expires_in = 3600;
            result.tokens.expires_at = unix_now() + expires_in;
            fill_identity_from_id_token(value.value("id_token", ""), result.tokens);
            return result;
        }

        const std::string upstream_error = value.value("error", "");
        result.error = upstream_error.empty()
            ? "GROK_TOKEN_EXCHANGE_FAILED"
            : redact_grok_auth_diagnostic(upstream_error);
        result.message = oauth_message(value);
        if (device_flow && upstream_error == "authorization_pending") {
            result.status = "pending";
            if (result.message.empty()) result.message = "Waiting for Grok authorization";
        } else if (device_flow && upstream_error == "slow_down") {
            result.status = "slow_down";
            result.interval_delta_seconds = 5;
            if (result.message.empty()) result.message = "Slow down Grok authorization polling";
        } else if (device_flow &&
                   (upstream_error == "expired_token" ||
                    upstream_error == "access_denied")) {
            result.status = "expired";
            if (result.message.empty()) result.message = "Grok device code expired or was denied";
        } else {
            result.status = "failed";
            if (result.message.empty()) result.message = "Grok token exchange failed";
        }
    } catch (const std::exception& e) {
        result.status = "failed";
        result.error = "GROK_TOKEN_BAD_JSON";
        result.message = e.what();
    }
    result.message = redact_grok_auth_diagnostic(result.message);
    return result;
}

std::vector<std::string> parse_grok_model_ids(
        const std::string& response_body,
        std::string* error) {
    std::set<std::string> unique;
    std::vector<std::string> models;
    try {
        const auto value = nlohmann::json::parse(response_body);
        const nlohmann::json* list = nullptr;
        if (value.is_array()) {
            list = &value;
        } else if (value.is_object() && value.contains("data") &&
                   value["data"].is_array()) {
            list = &value["data"];
        } else if (value.is_object() && value.contains("models") &&
                   value["models"].is_array()) {
            list = &value["models"];
        }
        if (!list) {
            if (error) *error = "Grok model response does not contain a model list";
            return {};
        }
        for (const auto& item : *list) {
            std::string id;
            if (item.is_string()) {
                id = item.get<std::string>();
            } else if (item.is_object()) {
                const auto meta_it = item.find("_meta");
                const bool hidden = item.value("hidden", false) ||
                    (meta_it != item.end() && meta_it->is_object() &&
                     meta_it->value("hidden", false));
                if (hidden) continue;

                for (const char* key : {"id", "model", "modelId"}) {
                    const auto it = item.find(key);
                    if (it != item.end() && it->is_string() && !it->get<std::string>().empty()) {
                        id = it->get<std::string>();
                        break;
                    }
                }
                if (id.empty() && meta_it != item.end() && meta_it->is_object()) {
                    for (const char* key : {"model", "modelId"}) {
                        const auto it = meta_it->find(key);
                        if (it != meta_it->end() && it->is_string() &&
                            !it->get<std::string>().empty()) {
                            id = it->get<std::string>();
                            break;
                        }
                    }
                }
            }
            if (!id.empty() && unique.insert(id).second) {
                models.push_back(std::move(id));
            }
        }
    } catch (const std::exception& e) {
        if (error) *error = redact_grok_auth_diagnostic(e.what());
        return {};
    }
    if (error) error->clear();
    return models;
}

GrokDeviceCodeResponse request_grok_device_code(const GrokAuthConfig& config) {
    auto proxy = network::proxy_options_for(config.device_url);
    const cpr::Response response = cpr::Post(
        cpr::Url{config.device_url},
        oauth_device_headers(config),
        cpr::Payload{
            {"client_id", config.client_id},
            {"scope", config.scope},
            {"referrer", "grok-build"},
        },
        network::build_ssl_options(proxy),
        proxy.proxies,
        proxy.auth,
        cpr::Timeout{config.timeout_ms});
    if (response.status_code == 0) {
        GrokDeviceCodeResponse result;
        result.error = "GROK_DEVICE_AUTH_UNREACHABLE";
        result.message = redact_grok_auth_diagnostic(response.error.message);
        return result;
    }
    return parse_grok_device_code_response(
        static_cast<int>(response.status_code), response.text);
}

GrokDevicePollResult poll_grok_device_code_once(
        const std::string& device_code,
        const GrokAuthConfig& config) {
    if (device_code.empty()) {
        GrokDevicePollResult result;
        result.status = "failed";
        result.status_code = 400;
        result.error = "GROK_DEVICE_CODE_REQUIRED";
        result.message = "device_code is required";
        return result;
    }

    // Serialize the whole poll-and-save operation with refresh and logout.
    // If logout starts while an in-flight poll is completing, it waits for
    // the save and then removes the newly issued credentials instead of
    // allowing them to reappear after the user disconnected.
    std::lock_guard<std::mutex> lock(g_grok_refresh_mutex);

    auto proxy = network::proxy_options_for(config.token_url);
    const cpr::Response response = cpr::Post(
        cpr::Url{config.token_url},
        oauth_device_headers(config),
        cpr::Payload{
            {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"},
            {"client_id", config.client_id},
            {"device_code", device_code},
        },
        network::build_ssl_options(proxy),
        proxy.proxies,
        proxy.auth,
        cpr::Timeout{config.timeout_ms});
    if (response.status_code == 0) {
        GrokDevicePollResult result;
        result.status = "failed";
        result.error = "GROK_AUTH_UNREACHABLE";
        result.message = redact_grok_auth_diagnostic(response.error.message);
        return result;
    }

    GrokDevicePollResult result = parse_grok_token_response(
        static_cast<int>(response.status_code), response.text, "", true);
    if (result.status == "authorized") {
        std::string save_error;
        if (!save_grok_auth_tokens(
                result.tokens, config.credential_path, &save_error)) {
            result.status = "failed";
            result.error = "GROK_AUTH_SAVE_FAILED";
            result.message = save_error;
        }
    }
    return result;
}

GrokAccessTokenResult ensure_grok_access_token(
        bool force_refresh,
        const std::string& rejected_access_token,
        const GrokAuthConfig& config) {
    std::lock_guard<std::mutex> lock(g_grok_refresh_mutex);
    GrokAccessTokenResult result;
    GrokAuthTokens current = load_grok_auth_tokens(config.credential_path);
    if (current.access_token.empty() && current.refresh_token.empty()) {
        result.status_code = 401;
        result.error = "GROK_AUTH_REQUIRED";
        result.message = "Grok Coding Plan authentication is required";
        return result;
    }

    const bool another_thread_rotated = force_refresh &&
        !rejected_access_token.empty() &&
        !current.access_token.empty() &&
        current.access_token != rejected_access_token;
    const bool fresh = !current.access_token.empty() &&
        current.expires_at > unix_now() + 60;
    if (another_thread_rotated || (!force_refresh && fresh)) {
        result.ok = true;
        result.status_code = 200;
        result.tokens = std::move(current);
        return result;
    }

    GrokDevicePollResult refreshed = refresh_grok_tokens(current, config);
    if (refreshed.status != "authorized") {
        result.status_code = refreshed.status_code == 0 ? 401 : refreshed.status_code;
        result.error = refreshed.error.empty()
            ? "GROK_REFRESH_FAILED"
            : refreshed.error;
        result.message = refreshed.message;
        return result;
    }

    std::string save_error;
    if (!save_grok_auth_tokens(
            refreshed.tokens, config.credential_path, &save_error)) {
        result.status_code = 500;
        result.error = "GROK_AUTH_SAVE_FAILED";
        result.message = save_error;
        return result;
    }
    result.ok = true;
    result.status_code = 200;
    result.tokens = std::move(refreshed.tokens);
    return result;
}

GrokModelsResult fetch_grok_model_ids(const GrokAuthConfig& config) {
    GrokAccessTokenResult access = ensure_grok_access_token(false, "", config);
    if (!access.ok) {
        return {{}, access.status_code, access.error, access.message};
    }
    GrokModelsResult result = fetch_grok_model_ids_once(config, access.tokens);
    if (result.status_code != 401) return result;

    GrokAccessTokenResult refreshed = ensure_grok_access_token(
        true, access.tokens.access_token, config);
    if (!refreshed.ok) {
        return {{}, refreshed.status_code, refreshed.error, refreshed.message};
    }
    return fetch_grok_model_ids_once(config, refreshed.tokens);
}

std::string grok_auth_file_path() {
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / "grok_auth.json");
}

GrokAuthTokens load_grok_auth_tokens(const std::string& path) {
    GrokAuthTokens tokens;
    std::ifstream input(path_from_utf8(auth_path_or_default(path)),
                        std::ios::binary);
    if (!input.is_open()) return tokens;
    try {
        nlohmann::json value;
        input >> value;
        tokens.access_token = value.value("access_token", "");
        tokens.refresh_token = value.value("refresh_token", "");
        tokens.expires_at = value.value("expires_at", int64_t{0});
        tokens.user_id = value.value("user_id", "");
        tokens.email = value.value("email", "");
    } catch (...) {
        return {};
    }
    return tokens;
}

bool save_grok_auth_tokens(const GrokAuthTokens& tokens,
                           const std::string& path,
                           std::string* error) {
    if (tokens.access_token.empty() || tokens.refresh_token.empty()) {
        if (error) *error = "Grok access and refresh tokens are required";
        return false;
    }
    const nlohmann::json value{
        {"access_token", tokens.access_token},
        {"refresh_token", tokens.refresh_token},
        {"expires_at", tokens.expires_at},
        {"user_id", tokens.user_id},
        {"email", tokens.email},
    };
    const std::string resolved_path = auth_path_or_default(path);
    if (!atomic_write_file(resolved_path, value.dump(2) + "\n", true)) {
        fs::path temp_path = path_from_utf8(resolved_path);
        temp_path += ".tmp";
        std::error_code cleanup_error;
        fs::remove(temp_path, cleanup_error);
        if (error) *error = "Could not write Grok credentials";
        return false;
    }
    if (error) error->clear();
    return true;
}

bool has_saved_grok_auth(const std::string& path) {
    const GrokAuthTokens tokens = load_grok_auth_tokens(path);
    return !tokens.access_token.empty() || !tokens.refresh_token.empty();
}

bool delete_grok_auth(const std::string& path, std::string* error) {
    std::lock_guard<std::mutex> lock(g_grok_refresh_mutex);
    const fs::path target = path_from_utf8(auth_path_or_default(path));
    fs::path temp_path = target;
    temp_path += ".tmp";

    std::error_code target_error;
    fs::remove(target, target_error);
    std::error_code temp_error;
    fs::remove(temp_path, temp_error);
    if (target_error || temp_error) {
        if (error) {
            *error = target_error ? target_error.message() : temp_error.message();
        }
        return false;
    }
    if (error) error->clear();
    return true;
}

} // namespace acecode
