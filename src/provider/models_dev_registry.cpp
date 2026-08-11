#include "models_dev_registry.hpp"
#include "models_dev_paths.hpp"

#include "../network/proxy_resolver.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#include <cpr/cpr.h>
#include <cpr/ssl_options.h>

namespace fs = std::filesystem;

namespace acecode {

namespace {

constexpr const char* kModelsDevUrl = "https://models.dev/api.json";

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

std::shared_ptr<const nlohmann::json>& registry_storage() {
    static std::shared_ptr<const nlohmann::json> r =
        std::make_shared<const nlohmann::json>(nlohmann::json::object());
    return r;
}

RegistrySource& source_storage() {
    static RegistrySource s;
    return s;
}

unsigned long long& generation_storage() {
    static unsigned long long generation = 0;
    return generation;
}

std::string lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

std::optional<nlohmann::json> read_json_file(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return std::nullopt;
    std::ifstream ifs(path);
    if (!ifs.is_open()) return std::nullopt;
    try {
        nlohmann::json j;
        ifs >> j;
        return j;
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("Failed to parse models.dev JSON at ") +
                  path_to_utf8(path) + ": " + e.what());
        return std::nullopt;
    }
}

void install(std::shared_ptr<const nlohmann::json> registry, RegistrySource src) {
    std::lock_guard<std::mutex> lk(registry_mutex());
    registry_storage() = std::move(registry);
    source_storage() = std::move(src);
    ++generation_storage();
}

} // namespace

bool validate_registry_schema(const nlohmann::json& registry) {
    if (!registry.is_object() || registry.empty()) return false;
    for (auto it = registry.begin(); it != registry.end(); ++it) {
        const auto& provider = it.value();
        if (!provider.is_object()) continue;
        auto models_it = provider.find("models");
        if (models_it == provider.end()) continue;
        if (models_it->is_object() && !models_it->empty()) return true;
        if (models_it->is_array() && !models_it->empty()) return true;
    }
    return false;
}

const nlohmann::json* find_provider_entry(const nlohmann::json& registry,
                                          const std::string& provider_id) {
    if (!registry.is_object() || provider_id.empty()) return nullptr;
    const std::string needle = lower(provider_id);
    for (auto it = registry.begin(); it != registry.end(); ++it) {
        if (lower(it.key()) == needle && it.value().is_object()) {
            return &it.value();
        }
    }
    return nullptr;
}

void initialize_registry(const AppConfig& cfg, const std::string& argv0_dir) {
    reload_registry_from_disk(cfg, argv0_dir);
}

void reload_registry_from_disk(const AppConfig& cfg, const std::string& argv0_dir) {
    const auto bundled_seed = find_models_dev_dir(argv0_dir);
    if (cfg.models_dev.user_override_path.has_value() &&
        !cfg.models_dev.user_override_path->empty()) {
        fs::path p = path_from_utf8(*cfg.models_dev.user_override_path);
        auto parsed = read_json_file(p);
        if (parsed) {
            if (!validate_registry_schema(*parsed)) {
                LOG_ERROR("models.dev user override at " + path_to_utf8(p) +
                          " failed schema validation; treating as empty");
                install(std::make_shared<const nlohmann::json>(nlohmann::json::object()),
                        RegistrySource{RegistrySource::Kind::Empty, path_to_utf8(p), std::nullopt, std::nullopt});
                return;
            }
            LOG_INFO("Loaded models.dev user override from " + path_to_utf8(p));
            install(std::make_shared<const nlohmann::json>(std::move(*parsed)),
                    RegistrySource{RegistrySource::Kind::UserOverride,
                                   path_to_utf8(p), std::nullopt, bundled_seed});
            return;
        }
        LOG_WARN("models.dev user_override_path '" + path_to_utf8(p) +
                 "' is missing or unreadable; falling back to bundled snapshot");
    }

    auto seed = bundled_seed;
    if (seed) {
        fs::path api_path = path_from_utf8(*seed) / "api.json";
        auto parsed = read_json_file(api_path);
        if (parsed) {
            std::optional<nlohmann::json> manifest =
                read_json_file(path_from_utf8(*seed) / "MANIFEST.json");

            if (!validate_registry_schema(*parsed)) {
                LOG_ERROR("Bundled models.dev registry at " + path_to_utf8(api_path) +
                          " failed schema validation; treating as empty");
                install(std::make_shared<const nlohmann::json>(nlohmann::json::object()),
                        RegistrySource{RegistrySource::Kind::Empty, path_to_utf8(api_path),
                                       std::move(manifest), seed});
                return;
            }
            LOG_INFO("Loaded bundled models.dev registry from " + path_to_utf8(api_path));
            install(std::make_shared<const nlohmann::json>(std::move(*parsed)),
                    RegistrySource{RegistrySource::Kind::Bundled, path_to_utf8(api_path),
                                   std::move(manifest), seed});
            return;
        }
    }

    LOG_WARN("models.dev registry not available (no user override, no bundled snapshot)");
    install(std::make_shared<const nlohmann::json>(nlohmann::json::object()),
            RegistrySource{RegistrySource::Kind::Empty, "", std::nullopt, std::nullopt});
}

bool refresh_registry_from_network() {
    auto proxy_opts = network::proxy_options_for(kModelsDevUrl);
    cpr::Response r = cpr::Get(
        cpr::Url{kModelsDevUrl},
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{20000}
    );
    if (r.status_code != 200) {
        LOG_INFO("models.dev network refresh failed (status=" +
                 std::to_string(r.status_code) + "), keeping current snapshot");
        return false;
    }
    try {
        auto parsed = nlohmann::json::parse(r.text);
        if (!install_registry_refresh_candidate(std::move(parsed), kModelsDevUrl)) {
            LOG_INFO("models.dev network response failed schema validation; keeping current snapshot");
            return false;
        }
        LOG_INFO("models.dev network refresh succeeded");
        return true;
    } catch (const std::exception& e) {
        LOG_INFO(std::string("models.dev network refresh parse error: ") + e.what());
        return false;
    }
}

std::shared_ptr<const nlohmann::json> current_registry() {
    std::lock_guard<std::mutex> lk(registry_mutex());
    return registry_storage();
}

RegistrySource current_registry_source() {
    std::lock_guard<std::mutex> lk(registry_mutex());
    return source_storage();
}

RegistrySnapshot current_registry_snapshot() {
    std::lock_guard<std::mutex> lk(registry_mutex());
    return RegistrySnapshot{
        registry_storage(), source_storage(), generation_storage()};
}

bool install_registry_refresh_candidate(nlohmann::json candidate,
                                        const std::string& source_url) {
    if (!validate_registry_schema(candidate)) return false;
    std::size_t provider_count = 0;
    std::size_t model_count = 0;
    for (auto it = candidate.begin(); it != candidate.end(); ++it) {
        if (!it->is_object()) continue;
        ++provider_count;
        const auto models = it->find("models");
        if (models != it->end() && (models->is_object() || models->is_array())) {
            model_count += models->size();
        }
    }
    // Match scripts/sync_models_dev.ps1. This threshold applies only to
    // network candidates; minimal local/user-override registries stay valid.
    if (provider_count < 50 || model_count < 1000) return false;
    const RegistrySource previous_source = current_registry_source();
    install(std::make_shared<const nlohmann::json>(std::move(candidate)),
            RegistrySource{RegistrySource::Kind::Network,
                           source_url,
                           previous_source.manifest,
                           previous_source.seed_dir});
    return true;
}

} // namespace acecode
