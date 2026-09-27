#include "model_probe_cache.hpp"

#include "utils/state_file.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <limits>
#include <set>

namespace acecode {

namespace {

constexpr const char* kModelProbeCacheKey = "model_probe_cache";
constexpr std::size_t kMaxModelProbeCacheEntries = 128;
constexpr std::size_t kMaxCachedProbeModels = 2000;
constexpr std::size_t kMaxCachedModelIdBytes = 512;

bool valid_sha256_fingerprint(const std::string& fingerprint) {
    if (fingerprint.size() != 64) return false;
    for (unsigned char c : fingerprint) {
        if (!std::isxdigit(c)) return false;
    }
    return true;
}

std::optional<ModelProbeCacheEntry> parse_model_probe_cache_entry(
    const nlohmann::json& value) {
    try {
        if (!value.is_object()) return std::nullopt;
        auto version = value.find("version");
        if (version == value.end() || !version->is_number_integer() ||
            version->get<std::int64_t>() != 1) {
            return std::nullopt;
        }
        auto models = value.find("models");
        if (models == value.end() || !models->is_array() ||
            models->size() > kMaxCachedProbeModels) {
            return std::nullopt;
        }

        ModelProbeCacheEntry entry;
        std::set<std::string> seen;
        for (const auto& model : *models) {
            if (!model.is_string()) return std::nullopt;
            const std::string id = model.get<std::string>();
            if (id.empty() || id.size() > kMaxCachedModelIdBytes) {
                return std::nullopt;
            }
            if (seen.insert(id).second) entry.models.push_back(id);
        }

        auto contexts = value.find("model_context_windows");
        if (contexts != value.end()) {
            if (!contexts->is_object()) return std::nullopt;
            for (auto it = contexts->begin(); it != contexts->end(); ++it) {
                if (!seen.count(it.key()) || !it.value().is_number_integer()) {
                    continue;
                }
                const auto context = it.value().get<std::int64_t>();
                if (context > 0 && context <= (std::numeric_limits<int>::max)()) {
                    entry.context_windows[it.key()] = static_cast<int>(context);
                }
            }
        }

        const auto reasonings = value.find("model_reasoning");
        for (const auto& id : entry.models) {
            entry.reasoning[id] = std::nullopt;
            if (reasonings == value.end() || !reasonings->is_object()) continue;
            const auto declaration = reasonings->find(id);
            if (declaration == reasonings->end() || !declaration->is_object()) continue;
            std::string error;
            auto parsed = parse_model_reasoning_options(*declaration, error);
            if (parsed.has_value() && parsed->supported &&
                !parsed->supported_efforts.empty()) {
                entry.reasoning[id] = std::move(parsed);
            }
        }

        auto probed_at = value.find("probed_at_ms");
        if (probed_at != value.end() && probed_at->is_number_integer()) {
            entry.probed_at_ms = probed_at->get<std::int64_t>();
        }
        return entry;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

nlohmann::json model_probe_cache_entry_to_json(
    const ModelProbeCacheEntry& source) {
    nlohmann::json value = {
        {"version", 1},
        {"models", nlohmann::json::array()},
        {"model_context_windows", nlohmann::json::object()},
        {"model_reasoning", nlohmann::json::object()},
        {"probed_at_ms", source.probed_at_ms},
    };
    std::set<std::string> seen;
    for (const auto& id : source.models) {
        if (id.empty() || id.size() > kMaxCachedModelIdBytes ||
            !seen.insert(id).second ||
            value["models"].size() >= kMaxCachedProbeModels) {
            continue;
        }
        value["models"].push_back(id);
        value["model_reasoning"][id] = nullptr;
        const auto declaration = source.reasoning.find(id);
        if (declaration != source.reasoning.end() && declaration->second.has_value()) {
            const auto encoded = model_reasoning_options_to_json(*declaration->second);
            std::string error;
            const auto parsed = parse_model_reasoning_options(encoded, error);
            if (parsed.has_value() && parsed->supported &&
                !parsed->supported_efforts.empty()) {
                value["model_reasoning"][id] = encoded;
            }
        }
    }
    for (const auto& [id, context] : source.context_windows) {
        if (context > 0 && seen.count(id)) {
            value["model_context_windows"][id] = context;
        }
    }
    return value;
}

} // namespace

std::optional<ModelProbeCacheEntry> read_model_probe_cache(
    const std::string& connection_fingerprint) {
    if (!valid_sha256_fingerprint(connection_fingerprint)) return std::nullopt;
    const auto state = read_state_json();
    auto cache = state.find(kModelProbeCacheKey);
    if (cache == state.end() || !cache->is_object()) return std::nullopt;
    auto entry = cache->find(connection_fingerprint);
    if (entry == cache->end()) return std::nullopt;
    return parse_model_probe_cache_entry(*entry);
}

bool write_model_probe_cache(
    const std::string& connection_fingerprint,
    const ModelProbeCacheEntry& entry) {
    if (!valid_sha256_fingerprint(connection_fingerprint)) return false;
    return update_state_json([connection_fingerprint, entry](nlohmann::json& state) {
        if (!state.contains(kModelProbeCacheKey) ||
            !state[kModelProbeCacheKey].is_object()) {
            state[kModelProbeCacheKey] = nlohmann::json::object();
        }
        auto& cache = state[kModelProbeCacheKey];
        cache[connection_fingerprint] = model_probe_cache_entry_to_json(entry);

        while (cache.size() > kMaxModelProbeCacheEntries) {
            auto oldest = cache.begin();
            std::int64_t oldest_time = (std::numeric_limits<std::int64_t>::max)();
            for (auto it = cache.begin(); it != cache.end(); ++it) {
                std::int64_t timestamp = 0;
                if (it.value().is_object()) {
                    auto field = it.value().find("probed_at_ms");
                    if (field != it.value().end() && field->is_number_integer()) {
                        timestamp = field->get<std::int64_t>();
                    }
                }
                if (timestamp < oldest_time) {
                    oldest = it;
                    oldest_time = timestamp;
                }
            }
            cache.erase(oldest);
        }
        return true;
    });
}

} // namespace acecode
