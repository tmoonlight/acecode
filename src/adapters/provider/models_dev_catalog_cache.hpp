#pragma once

// models.dev 目录对全局 registry 的缓存视图(P2-05 自 utils/models_dev_catalog 拆出):
// 缓存按 current_registry() 的 shared_ptr 身份失效,依赖 provider 的 models_dev_registry,
// 所以留在 adapters 层;纯目录构造与格式化在 config/models_dev_catalog.hpp。

#include "config/models_dev_catalog.hpp"

#include <string>
#include <vector>

namespace acecode {

// Cached view over the global registry (current_registry()). The cache is keyed
// off the shared_ptr identity, so calling this after refresh_registry_*() picks
// up the new data automatically.
const std::vector<ProviderEntry>& all_providers();

// Same as all_providers(), filtered to providers whose base_url is set.
std::vector<const ProviderEntry*> openai_compat_providers();

const ProviderEntry* find_provider(const std::string& id);

// Version of the catalog cache. Increments whenever the cache is rebuilt — used
// by tests and by long-lived UI components that want to invalidate state.
unsigned long long catalog_version();

} // namespace acecode
