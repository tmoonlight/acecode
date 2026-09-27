#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode {

struct ModelEntry {
    std::string id;
    std::string name;                              // human label (falls back to id)
    std::optional<int> context;
    std::optional<int> max_output;
    std::optional<double> cost_input;              // USD / million tokens
    std::optional<double> cost_output;
    bool reasoning = false;
    std::vector<std::string> reasoning_efforts;
    bool reasoning_supports_max_tokens = false;
    bool reasoning_can_disable = false;
    bool tool_call = false;
    bool attachment = false;                       // vision / pdf
    bool deprecated = false;
    std::vector<std::string> input_modalities;
    std::vector<std::string> output_modalities;
    std::optional<std::string> knowledge_cutoff;
};

struct ProviderEntry {
    std::string id;                                // top-level key from api.json
    std::string name;
    std::vector<std::string> env;                  // suggested env var names
    std::optional<std::string> base_url;           // openai-compatible endpoint, when present
    std::optional<std::string> doc;
    bool openai_compatible = false;                // true iff base_url present
    std::vector<ModelEntry> models;
};

// Build a fresh catalog from the supplied registry JSON. Pure function — exposed
// for unit testing and for callers that want to render a catalog from a JSON
// blob other than the global registry.
std::vector<ProviderEntry> build_catalog(const nlohmann::json& registry);

// 全局 registry 的缓存视图(all_providers / find_provider / catalog_version …)在
// provider/models_dev_catalog_cache.hpp(P2-05):它依赖 provider 的 models_dev_registry,
// 这里只留纯函数。

const ModelEntry* find_model(const ProviderEntry& provider, const std::string& model_id);

// Display helpers (safe to call with empty entries).
std::string format_context(const std::optional<int>& tokens);
std::string format_cost(const std::optional<double>& input,
                        const std::optional<double>& output);
// Canonical saved-profile capability ids derived from catalog metadata.
std::vector<std::string> model_capability_tags(const ModelEntry& model);
std::string format_capabilities(const ModelEntry& model);

} // namespace acecode
