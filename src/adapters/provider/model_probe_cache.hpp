#pragma once

// provider 模型探测结果的 state.json 缓存(P2-05 自 utils/state_file 拆出):它的条目类型依赖
// config/saved_models 的 ModelReasoningOptions,留在 utils 会让 utils 向上依赖 config;读写经
// state_file 的通用入口(read_state_json / update_state_json),锁与原子写的顺序不变。

#include "config/saved_models.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

// Successful Provider model probes are cached by an opaque SHA-256 connection
// fingerprint. The state file stores only the fingerprint and probe output;
// provider URLs, API keys, and request headers never reach this helper.
struct ModelProbeCacheEntry {
    std::vector<std::string> models;
    std::map<std::string, int> context_windows;
    std::int64_t probed_at_ms = 0;
    std::map<std::string, std::optional<ModelReasoningOptions>> reasoning;
};

// Missing/malformed entries return nullopt. The fingerprint must be exactly a
// 64-character hexadecimal SHA-256 digest.
std::optional<ModelProbeCacheEntry> read_model_probe_cache(
    const std::string& connection_fingerprint);

// Atomically upserts one cache entry while preserving unrelated state and
// other connection fingerprints. Returns false for an invalid fingerprint or
// durable write failure.
bool write_model_probe_cache(
    const std::string& connection_fingerprint,
    const ModelProbeCacheEntry& entry);

} // namespace acecode
