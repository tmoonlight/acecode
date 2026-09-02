#include "model_context_resolver.hpp"

#include "../network/proxy_resolver.hpp"
#include "../utils/logger.hpp"
#include "builtin_model_catalog.hpp"
#include "codex/codex_model_catalog.hpp"
#include "model_context_metadata.hpp"
#include "models_dev_registry.hpp"

#include <cpr/cpr.h>
#include <cpr/ssl_options.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace acecode {
namespace {

std::mutex g_context_cache_mu;
std::map<std::string, int> g_context_cache;
std::set<std::string> g_context_probe_in_flight;

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

int lookup_models_dev_context(const std::string& provider_id, const std::string& model) {
    if (provider_id.empty() || model.empty()) {
        return 0;
    }

    if (is_acemodel_provider_id(provider_id)) {
        const ModelEntry* entry = find_acemodel_catalog_model(model);
        if (!entry || !entry->context.has_value() || *entry->context <= 0) return 0;
        LOG_INFO("Resolved model context via ACEModel built-in catalog model=" + model +
                 " context=" + std::to_string(*entry->context));
        return *entry->context;
    }

    auto registry = current_registry();
    if (!registry) return 0;

    const nlohmann::json* provider = find_provider_entry(*registry, provider_id);
    if (!provider) return 0;

    auto models_it = provider->find("models");
    if (models_it == provider->end()) {
        return 0;
    }

    const nlohmann::json* entry = find_model_entry(*models_it, model);
    if (!entry) {
        return 0;
    }

    int context = model_context_window_from_metadata(*entry);
    if (context > 0) {
        LOG_INFO("Resolved model context via models.dev provider=" + provider_id +
                 " model=" + model + " context=" + std::to_string(context));
    }
    return context;
}

int fetch_models_endpoint_context(const std::string& base_url,
                                  const std::string& api_key,
                                  const std::string& model,
                                  const std::atomic<bool>* cancel_requested = nullptr,
                                  const network::ProxyOptions* prepared_proxy = nullptr) {
    if (base_url.empty() || model.empty()) {
        return 0;
    }

    const std::string url = trim_trailing_slash(base_url) + "/models";
    cpr::Header headers = {{"Content-Type", "application/json"}};
    if (!api_key.empty()) {
        headers["Authorization"] = "Bearer " + api_key;
    }

    network::ProxyOptions resolved_proxy;
    if (!prepared_proxy) {
        resolved_proxy = network::proxy_options_for(url);
    }
    const auto& proxy_opts = prepared_proxy ? *prepared_proxy : resolved_proxy;
    auto progress_cb = cpr::ProgressCallback{
        [cancel_requested](cpr::cpr_off_t,
                           cpr::cpr_off_t,
                           cpr::cpr_off_t,
                           cpr::cpr_off_t,
                           intptr_t) -> bool {
            return !cancel_requested || !cancel_requested->load();
        }
    };
    cpr::Response response = cpr::Get(
        cpr::Url{url},
        headers,
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        progress_cb,
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

        int context = model_context_window_from_metadata(*entry);
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
    // Explicit hint from configure wizard / catalog selection wins over heuristics.
    if (config.openai.models_dev_provider_id.has_value() &&
        !config.openai.models_dev_provider_id->empty()) {
        return *config.openai.models_dev_provider_id;
    }

    const std::string normalized_provider = to_lower_copy(provider_name.empty() ? config.provider : provider_name);
    if (normalized_provider == "copilot") {
        return "github-copilot";
    }
    if (normalized_provider == "grok") {
        return "xai";
    }

    if (normalized_provider == "openai") {
        const std::string base_url = to_lower_copy(config.openai.base_url);
        if (base_url.find("api.openai.com") != std::string::npos) {
            return "openai";
        }
    }

    // The provider_name argument is also accepted as a direct models.dev id
    // (e.g. "anthropic", "openrouter") so callers can short-circuit detection.
    if (!normalized_provider.empty() && normalized_provider != "openai" &&
        normalized_provider != "copilot" &&
        normalized_provider != "grok") {
        return normalized_provider;
    }

    return "";
}

std::string context_cache_key(const AppConfig& config,
                              const std::string& provider_name,
                              const std::string& model) {
    const std::string normalized_provider = to_lower_copy(
        provider_name.empty() ? config.provider : provider_name);
    std::string base_url;
    std::string provider_hint;
    if (normalized_provider == "openai") {
        base_url = to_lower_copy(trim_trailing_slash(config.openai.base_url));
        if (config.openai.models_dev_provider_id.has_value()) {
            provider_hint = to_lower_copy(*config.openai.models_dev_provider_id);
        }
    }
    return normalized_provider + "\n" + to_lower_copy(model) + "\n" +
           base_url + "\n" + provider_hint;
}

AppConfig config_for_profile_context(const AppConfig& cfg,
                                     const ModelProfile& profile) {
    AppConfig context_cfg = cfg;
    context_cfg.provider = profile.provider;
    if (profile.provider == "openai") {
        context_cfg.openai.base_url = profile.base_url;
        context_cfg.openai.api_key = profile.api_key;
        context_cfg.openai.model = profile.model;
        context_cfg.openai.models_dev_provider_id = profile.models_dev_provider_id;
    } else if (profile.provider == "anthropic") {
        context_cfg.openai.models_dev_provider_id =
            profile.models_dev_provider_id.has_value()
                ? profile.models_dev_provider_id
                : std::optional<std::string>{"anthropic"};
    } else if (profile.provider == "grok") {
        context_cfg.openai.models_dev_provider_id =
            profile.models_dev_provider_id.has_value()
                ? profile.models_dev_provider_id
                : std::optional<std::string>{"xai"};
    } else if (profile.provider == "codex") {
        context_cfg.codex.model = profile.model;
    } else {
        context_cfg.copilot.model = profile.model;
    }
    return context_cfg;
}

int cached_context(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_context_cache_mu);
    auto it = g_context_cache.find(key);
    return it == g_context_cache.end() ? 0 : it->second;
}

void remember_context(const std::string& key, int context) {
    if (context <= 0) return;
    std::lock_guard<std::mutex> lk(g_context_cache_mu);
    g_context_cache[key] = context;
}

int acemodel_fallback_context(const AppConfig& config,
                              const std::string& provider_name,
                              const std::string& model) {
    const std::string normalized_provider = to_lower_copy(
        provider_name.empty() ? config.provider : provider_name);
    if (normalized_provider != "openai") return 0;

    const bool catalog_identity =
        config.openai.models_dev_provider_id.has_value() &&
        is_acemodel_provider_id(*config.openai.models_dev_provider_id);
    if (!catalog_identity && !is_acemodel_base_url(config.openai.base_url)) return 0;

    const ModelEntry* entry = find_acemodel_catalog_model(model);
    if (!entry || !entry->context.has_value() || *entry->context <= 0) return 0;
    return *entry->context;
}

bool profile_has_authoritative_context_override(const ModelProfile& profile) {
    return profile.context_window.has_value() && *profile.context_window > 0 &&
           !is_acemodel_catalog_context_fallback(profile);
}

int cached_or_local_context(const AppConfig& config,
                            const std::string& provider_name,
                            const std::string& model) {
    const std::string key = context_cache_key(config, provider_name, model);
    if (int context = cached_context(key); context > 0) {
        return context;
    }

    const std::string normalized_provider = to_lower_copy(
        provider_name.empty() ? config.provider : provider_name);
    if (normalized_provider == "codex") {
        int context = codex::context_window_for_model(model);
        if (context > 0) {
            remember_context(key, context);
            return context;
        }
    }

    // ACEModel's built-in value is only a fallback. Returning and caching it
    // here would prevent both blocking and background `/models` probes from
    // ever observing the server-provided context window.
    if (acemodel_fallback_context(config, provider_name, model) > 0) return 0;

    const std::string models_dev_provider = detect_models_dev_provider(config, provider_name);
    if (!models_dev_provider.empty()) {
        int context = lookup_models_dev_context(models_dev_provider, model);
        if (context > 0) {
            remember_context(key, context);
            return context;
        }
    }

    return 0;
}

bool mark_probe_in_flight(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_context_cache_mu);
    if (g_context_cache.find(key) != g_context_cache.end()) return false;
    return g_context_probe_in_flight.insert(key).second;
}

void clear_probe_in_flight(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_context_cache_mu);
    g_context_probe_in_flight.erase(key);
}

struct ContextProbeTask {
    AppConfig config;
    std::string model;
    std::string key;
    network::ProxyOptions proxy_options;
    std::shared_ptr<std::atomic<bool>> cancel_requested =
        std::make_shared<std::atomic<bool>>(false);
};

class ContextProbeService {
public:
    ContextProbeService()
        : worker_([this] { run(); }) {}

    ~ContextProbeService() {
        stop();
    }

    ContextProbeService(const ContextProbeService&) = delete;
    ContextProbeService& operator=(const ContextProbeService&) = delete;

    bool enqueue(ContextProbeTask task) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stopping_) return false;
            tasks_.push_back(std::move(task));
        }
        work_cv_.notify_one();
        return true;
    }

    void cancel_and_wait() {
        std::deque<ContextProbeTask> discarded;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (active_cancel_) active_cancel_->store(true);
            discarded.swap(tasks_);
        }
        for (const auto& task : discarded) {
            clear_probe_in_flight(task.key);
        }
        work_cv_.notify_all();

        std::unique_lock<std::mutex> lk(mu_);
        idle_cv_.wait(lk, [this] {
            return !active_ && tasks_.empty();
        });
    }

private:
    void stop() {
        std::deque<ContextProbeTask> discarded;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stopping_) return;
            stopping_ = true;
            if (active_cancel_) active_cancel_->store(true);
            discarded.swap(tasks_);
        }
        for (const auto& task : discarded) {
            clear_probe_in_flight(task.key);
        }
        work_cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    void run() {
        while (true) {
            ContextProbeTask task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                work_cv_.wait(lk, [this] {
                    return stopping_ || !tasks_.empty();
                });
                if (stopping_) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
                active_ = true;
                active_cancel_ = task.cancel_requested;
            }

            try {
                int context = fetch_models_endpoint_context(
                    task.config.openai.base_url,
                    task.config.openai.api_key,
                    task.model,
                    task.cancel_requested.get(),
                    &task.proxy_options);
                if (context > 0 && !task.cancel_requested->load()) {
                    remember_context(task.key, context);
                }
            } catch (const std::exception& ex) {
                if (!task.cancel_requested->load()) {
                    LOG_WARN(std::string("Background model context probe failed: ") +
                             ex.what());
                }
            } catch (...) {
                if (!task.cancel_requested->load()) {
                    LOG_WARN("Background model context probe failed with unknown error");
                }
            }
            clear_probe_in_flight(task.key);

            {
                std::lock_guard<std::mutex> lk(mu_);
                active_ = false;
                active_cancel_.reset();
            }
            idle_cv_.notify_all();
        }
    }

    std::mutex mu_;
    std::condition_variable work_cv_;
    std::condition_variable idle_cv_;
    std::deque<ContextProbeTask> tasks_;
    std::shared_ptr<std::atomic<bool>> active_cancel_;
    std::thread worker_;
    bool active_ = false;
    bool stopping_ = false;
};

ContextProbeService& context_probe_service() {
    // These function-local dependencies must be constructed before the probe
    // service so reverse static destruction stops and joins the worker first.
    (void)network::proxy_resolver();
    (void)Logger::instance();
    static const bool cpr_runtime_initialized = [] {
        cpr::Session session;
        return true;
    }();
    (void)cpr_runtime_initialized;

    static ContextProbeService service;
    return service;
}

void warm_context_async(AppConfig config,
                        std::string provider_name,
                        std::string model) {
    const std::string normalized_provider = to_lower_copy(
        provider_name.empty() ? config.provider : provider_name);
    if (normalized_provider != "openai" || config.openai.base_url.empty() || model.empty()) {
        return;
    }

    const std::string key = context_cache_key(config, provider_name, model);
    if (!mark_probe_in_flight(key)) return;

    try {
        const std::string url = trim_trailing_slash(config.openai.base_url) + "/models";
        ContextProbeTask task;
        task.config = std::move(config);
        task.model = std::move(model);
        task.key = key;
        task.proxy_options = network::proxy_options_for(url);
        if (context_probe_service().enqueue(std::move(task))) return;
    } catch (const std::exception& ex) {
        LOG_WARN(std::string("Failed to queue background model context probe: ") +
                 ex.what());
    } catch (...) {
        LOG_WARN("Failed to queue background model context probe with unknown error");
    }

    clear_probe_in_flight(key);
}

} // namespace

int resolve_model_context_window(const AppConfig& config,
                                 const std::string& provider_name,
                                 const std::string& model,
                                 int fallback_context_window) {
    const std::string key = context_cache_key(config, provider_name, model);
    const int acemodel_fallback =
        acemodel_fallback_context(config, provider_name, model);
    if (int context = cached_or_local_context(config, provider_name, model); context > 0) {
        return context;
    }

    const std::string normalized_provider = to_lower_copy(provider_name.empty() ? config.provider : provider_name);
    if (normalized_provider == "openai") {
        int context = fetch_models_endpoint_context(config.openai.base_url, config.openai.api_key, model);
        if (context > 0) {
            remember_context(key, context);
            return context;
        }
    }

    if (acemodel_fallback > 0) return acemodel_fallback;
    return fallback_context_window;
}

int resolve_model_context_window_nonblocking(const AppConfig& config,
                                             const std::string& provider_name,
                                             const std::string& model,
                                             int fallback_context_window) {
    const int acemodel_fallback =
        acemodel_fallback_context(config, provider_name, model);
    if (int context = cached_or_local_context(config, provider_name, model); context > 0) {
        return context;
    }
    warm_context_async(config, provider_name, model);
    if (acemodel_fallback > 0) return acemodel_fallback;
    return fallback_context_window;
}

int resolve_model_profile_context_window(const AppConfig& config,
                                         const ModelProfile& profile,
                                         int fallback_context_window) {
    if (profile_has_authoritative_context_override(profile)) {
        return *profile.context_window;
    }
    auto context_cfg = config_for_profile_context(config, profile);
    return resolve_model_context_window(
        context_cfg, profile.provider, profile.model, fallback_context_window);
}

int resolve_model_profile_context_window_nonblocking(const AppConfig& config,
                                                     const ModelProfile& profile,
                                                     int fallback_context_window) {
    if (profile_has_authoritative_context_override(profile)) {
        return *profile.context_window;
    }
    auto context_cfg = config_for_profile_context(config, profile);
    return resolve_model_context_window_nonblocking(
        context_cfg, profile.provider, profile.model, fallback_context_window);
}

int resolve_runtime_model_profile_context_window_nonblocking(
    const AppConfig& config,
    const ModelProfile& profile,
    int fallback_context_window,
    int model_pool_context_window) {
    const int resolved = resolve_model_profile_context_window_nonblocking(
        config, profile, fallback_context_window);
    if (profile_has_authoritative_context_override(profile)) {
        return resolved;
    }
    return model_pool_context_window > 0 ? model_pool_context_window : resolved;
}

void reset_model_context_window_cache_for_test() {
    context_probe_service().cancel_and_wait();
    std::lock_guard<std::mutex> lk(g_context_cache_mu);
    g_context_cache.clear();
    g_context_probe_in_flight.clear();
}

} // namespace acecode
