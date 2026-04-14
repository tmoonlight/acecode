#include "model_context_resolver.hpp"

#include "../utils/logger.hpp"

#include <cpr/cpr.h>
#include <cpr/ssl_options.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace acecode {
namespace {

constexpr const char* kModelsDevUrl = "https://models.dev/api.json";

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trim_trailing_slash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

std::string strip_provider_prefix(const std::string& model) {
    auto pos = model.find('/');
    if (pos == std::string::npos || pos + 1 >= model.size()) {
        return model;
    }
    return model.substr(pos + 1);
}

bool model_matches(const std::string& candidate, const std::string& target) {
    if (candidate.empty() || target.empty()) {
        return false;
    }

    const std::string normalized_candidate = to_lower_copy(candidate);
    const std::string normalized_target = to_lower_copy(target);
    if (normalized_candidate == normalized_target) {
        return true;
    }

    return strip_provider_prefix(normalized_candidate) == strip_provider_prefix(normalized_target);
}

int json_int_value(const nlohmann::json& value) {
    if (value.is_number_integer()) {
        return value.get<int>();
    }
    if (value.is_number_unsigned()) {
        return static_cast<int>(value.get<unsigned int>());
    }
    if (value.is_number_float()) {
        return static_cast<int>(value.get<double>());
    }
    return 0;
}

int extract_context_length(const nlohmann::json& value) {
    static const std::vector<std::string> keys = {
        "context_length",
        "context_window",
        "max_context_length",
        "max_input_tokens",
        "input_token_limit",
        "input_tokens",
        "context",
        "input"
    };

    if (value.is_object()) {
        for (const auto& key : keys) {
            auto it = value.find(key);
            if (it != value.end()) {
                int parsed = json_int_value(*it);
                if (parsed > 0) {
                    return parsed;
                }
            }
        }

        for (const auto& item : value.items()) {
            int parsed = extract_context_length(item.value());
            if (parsed > 0) {
                return parsed;
            }
        }
    }

    if (value.is_array()) {
        for (const auto& item : value) {
            int parsed = extract_context_length(item);
            if (parsed > 0) {
                return parsed;
            }
        }
    }

    return 0;
}

const nlohmann::json* find_model_entry(const nlohmann::json& models, const std::string& model) {
    if (models.is_array()) {
        for (const auto& item : models) {
            if (!item.is_object()) {
                continue;
            }
            if (model_matches(item.value("id", ""), model)) {
                return &item;
            }
        }
    }

    if (models.is_object()) {
        for (const auto& item : models.items()) {
            if (model_matches(item.key(), model)) {
                return &item.value();
            }
            if (item.value().is_object() && model_matches(item.value().value("id", ""), model)) {
                return &item.value();
            }
        }
    }

    return nullptr;
}

const nlohmann::json& models_dev_cache() {
    static std::mutex mutex;
    static nlohmann::json cached = nlohmann::json::object();
    static bool loaded = false;

    std::lock_guard<std::mutex> lock(mutex);
    if (loaded) {
        return cached;
    }

    loaded = true;

    cpr::Response response = cpr::Get(
        cpr::Url{kModelsDevUrl},
        cpr::Ssl(cpr::ssl::NoRevoke{true}),
        cpr::Timeout{20000}
    );

    if (response.status_code != 200) {
        LOG_WARN("models.dev fetch failed, status=" + std::to_string(response.status_code));
        return cached;
    }

    try {
        cached = nlohmann::json::parse(response.text);
    } catch (const std::exception& ex) {
        LOG_WARN(std::string("models.dev parse failed: ") + ex.what());
        cached = nlohmann::json::object();
    }

    return cached;
}

int lookup_models_dev_context(const std::string& provider_id, const std::string& model) {
    if (provider_id.empty() || model.empty()) {
        return 0;
    }

    const auto& cache = models_dev_cache();
    auto provider_it = cache.find(provider_id);
    if (provider_it == cache.end() || !provider_it->is_object()) {
        return 0;
    }

    auto models_it = provider_it->find("models");
    if (models_it == provider_it->end()) {
        return 0;
    }

    const nlohmann::json* entry = find_model_entry(*models_it, model);
    if (!entry) {
        return 0;
    }

    int context = extract_context_length(*entry);
    if (context > 0) {
        LOG_INFO("Resolved model context via models.dev provider=" + provider_id +
                 " model=" + model + " context=" + std::to_string(context));
    }
    return context;
}

int fetch_models_endpoint_context(const std::string& base_url,
                                  const std::string& api_key,
                                  const std::string& model) {
    if (base_url.empty() || model.empty()) {
        return 0;
    }

    const std::string url = trim_trailing_slash(base_url) + "/models";
    cpr::Header headers = {{"Content-Type", "application/json"}};
    if (!api_key.empty()) {
        headers["Authorization"] = "Bearer " + api_key;
    }

    cpr::Response response = cpr::Get(
        cpr::Url{url},
        headers,
        cpr::Ssl(cpr::ssl::NoRevoke{true}),
        cpr::Timeout{15000}
    );

    if (response.status_code != 200) {
        return 0;
    }

    try {
        nlohmann::json parsed = nlohmann::json::parse(response.text);
        const nlohmann::json* entry = nullptr;

        if (parsed.is_object() && parsed.contains("data")) {
            entry = find_model_entry(parsed["data"], model);
        }
        if (!entry) {
            entry = find_model_entry(parsed, model);
        }
        if (!entry) {
            return 0;
        }

        int context = extract_context_length(*entry);
        if (context > 0) {
            LOG_INFO("Resolved model context via endpoint model=" + model +
                     " context=" + std::to_string(context));
        }
        return context;
    } catch (const std::exception& ex) {
        LOG_WARN(std::string("Failed to parse /models metadata: ") + ex.what());
        return 0;
    }
}

std::string detect_models_dev_provider(const AppConfig& config, const std::string& provider_name) {
    const std::string normalized_provider = to_lower_copy(provider_name.empty() ? config.provider : provider_name);
    if (normalized_provider == "copilot") {
        return "github-copilot";
    }

    if (normalized_provider == "openai") {
        const std::string base_url = to_lower_copy(config.openai.base_url);
        if (base_url.find("api.openai.com") != std::string::npos) {
            return "openai";
        }
    }

    return "";
}

} // namespace

int resolve_model_context_window(const AppConfig& config,
                                 const std::string& provider_name,
                                 const std::string& model,
                                 int fallback_context_window) {
    const std::string models_dev_provider = detect_models_dev_provider(config, provider_name);
    if (!models_dev_provider.empty()) {
        int context = lookup_models_dev_context(models_dev_provider, model);
        if (context > 0) {
            return context;
        }
    }

    const std::string normalized_provider = to_lower_copy(provider_name.empty() ? config.provider : provider_name);
    if (normalized_provider == "openai") {
        int context = fetch_models_endpoint_context(config.openai.base_url, config.openai.api_key, model);
        if (context > 0) {
            return context;
        }
    }

    return fallback_context_window;
}

} // namespace acecode