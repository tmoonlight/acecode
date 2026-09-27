#include "models_dev_catalog_cache.hpp"

#include "models_dev_registry.hpp"

#include <algorithm>
#include <memory>
#include <mutex>

namespace acecode {

namespace {

struct CatalogCache {
    std::shared_ptr<const nlohmann::json> source;
    std::vector<ProviderEntry> providers;
    unsigned long long version = 0;
};

std::mutex& cache_mutex() {
    static std::mutex m;
    return m;
}

CatalogCache& cache_storage() {
    static CatalogCache c;
    return c;
}

const std::vector<ProviderEntry>& refresh_cache_if_stale() {
    std::lock_guard<std::mutex> lk(cache_mutex());
    auto current = current_registry();
    auto& cache = cache_storage();
    if (cache.source.get() == current.get() && cache.source) {
        return cache.providers;
    }
    cache.source = current;
    cache.providers = current ? build_catalog(*current) : std::vector<ProviderEntry>{};
    std::sort(cache.providers.begin(), cache.providers.end(),
              [](const ProviderEntry& a, const ProviderEntry& b) {
                  return a.name < b.name;
              });
    ++cache.version;
    return cache.providers;
}

} // namespace

const std::vector<ProviderEntry>& all_providers() {
    return refresh_cache_if_stale();
}

std::vector<const ProviderEntry*> openai_compat_providers() {
    const auto& providers = all_providers();
    std::vector<const ProviderEntry*> out;
    out.reserve(providers.size());
    for (const auto& p : providers) {
        if (p.openai_compatible) out.push_back(&p);
    }
    return out;
}

const ProviderEntry* find_provider(const std::string& id) {
    for (const auto& p : all_providers()) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

unsigned long long catalog_version() {
    refresh_cache_if_stale();
    std::lock_guard<std::mutex> lk(cache_mutex());
    return cache_storage().version;
}

} // namespace acecode
