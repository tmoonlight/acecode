#include "config.hpp"
#include "utils/http_url_validation.hpp"

#include <algorithm>
#include <cctype>

namespace acecode {
namespace {

bool valid_url(const std::string& url) {
    if (!utils::is_valid_http_base_url(url, true, true) || url.find('\\') != std::string::npos) return false;
    const auto begin = url.find("://") + 3;
    const auto end = url.find('/', begin);
    const auto authority = url.substr(begin, end == std::string::npos ? end : end - begin);
    std::size_t port_at = authority.find(':');
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos || close == 1) return false;
        if (close + 1 == authority.size()) return true;
        if (authority[close + 1] != ':') return false;
        port_at = close + 1;
    }
    if (port_at == std::string::npos) return true;
    if (port_at == 0) return false;
    const auto port = authority.substr(port_at + 1);
    if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(),
        [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) return false;
    const int number = std::stoi(port);
    return number > 0 && number <= 65535;
}

bool clean_text(const std::string& value) {
    return std::none_of(value.begin(), value.end(),
        [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}

} // namespace

bool apply_speech_recognition_config(SpeechRecognitionConfig& config,
                                     const nlohmann::json& patch, std::string& error) {
    error.clear();
    if (!patch.is_object()) { error = "speech_recognition must be an object"; return false; }
    auto next = config;
    const auto boolean = [&](const char* key, bool& target) {
        if (!patch.contains(key)) return true;
        if (!patch[key].is_boolean()) { error = std::string(key) + " must be boolean"; return false; }
        target = patch[key].get<bool>();
        return true;
    };
    const auto text = [&](const char* key, std::string& target, std::size_t limit) {
        if (!patch.contains(key)) return true;
        if (!patch[key].is_string()) { error = std::string(key) + " must be text"; return false; }
        const auto value = patch[key].get<std::string>();
        if (value.size() > limit || !clean_text(value)) { error = std::string(key) + " is invalid"; return false; }
        target = value;
        return true;
    };
    const auto number = [&](const char* key, int& target, int low, int high) {
        if (!patch.contains(key)) return true;
        if (!patch[key].is_number_integer() || patch[key].get<double>() < low || patch[key].get<double>() > high) {
            error = std::string(key) + " is out of range"; return false;
        }
        target = patch[key].get<int>();
        return true;
    };
    if (!boolean("enabled", next.enabled) || !boolean("use_proxy", next.use_proxy) ||
        !text("protocol", next.protocol, 32) || !text("base_url", next.base_url, 2048) ||
        !text("model", next.model, 256) || !text("api_key", next.api_key, 16384) ||
        !text("language", next.language, 32) ||
        !number("timeout_ms", next.timeout_ms, 5000, 600000) ||
        !number("max_recording_seconds", next.max_recording_seconds, 1, 300)) return false;
    if (next.protocol != "sensevoice" && next.protocol != "openai" && next.protocol != "whisper_cpp") {
        error = "unsupported speech protocol"; return false;
    }
    while (!next.base_url.empty() && next.base_url.back() == '/') next.base_url.pop_back();
    if (!next.base_url.empty() && !valid_url(next.base_url)) {
        error = "base_url must be an HTTP/HTTPS service URL with a valid port, without credentials, query or fragment";
        return false;
    }
    if (next.api_key.find(' ') != std::string::npos) { error = "api_key contains spaces"; return false; }
    config = std::move(next);
    return true;
}

nlohmann::json speech_recognition_config_json(const SpeechRecognitionConfig& config) {
    return {{"enabled", config.enabled}, {"protocol", config.protocol},
            {"base_url", config.base_url}, {"model", config.model}, {"api_key", config.api_key},
            {"language", config.language}, {"timeout_ms", config.timeout_ms},
            {"max_recording_seconds", config.max_recording_seconds}, {"use_proxy", config.use_proxy}};
}

bool speech_recognition_configured(const SpeechRecognitionConfig& config) {
    auto copy = config;
    std::string error;
    return apply_speech_recognition_config(copy, nlohmann::json::object(), error) &&
        !copy.base_url.empty() && (copy.protocol != "openai" || !copy.model.empty());
}

} // namespace acecode
