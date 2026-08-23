#include "builtin_model_catalog.hpp"

#include <algorithm>
#include <cctype>

namespace acecode {
namespace {

constexpr int kAceModelContextWindow = 200000;

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

std::string normalize_base_url(std::string value) {
    std::size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    value = value.substr(start, end - start);
    while (!value.empty() && value.back() == '/') value.pop_back();
    return lower_ascii(value);
}

ModelEntry builtin_model(const std::string& id, const std::string& name) {
    ModelEntry model;
    model.id = id;
    model.name = name;
    model.context = kAceModelContextWindow;
    model.tool_call = true;
    return model;
}

ProviderEntry build_acemodel_provider() {
    ProviderEntry provider;
    provider.id = "acemodel";
    provider.name = "ACEModel";
    provider.env = {"ACEMODEL_API_KEY"};
    provider.base_url = "https://ge.bigjuan.xyz/aceapi/v1";
    provider.openai_compatible = true;
    provider.models = {
        builtin_model("moonlight", "Moonlight"),
        builtin_model("starrylight", "Starrylight"),
        builtin_model("aurora", "Aurora"),
    };
    return provider;
}

} // namespace

const ProviderEntry& acemodel_catalog_provider() {
    static const ProviderEntry provider = build_acemodel_provider();
    return provider;
}

bool is_acemodel_provider_id(const std::string& provider_id) {
    return lower_ascii(provider_id) == acemodel_catalog_provider().id;
}

const ModelEntry* find_acemodel_catalog_model(const std::string& model_id) {
    const std::string normalized = lower_ascii(model_id);
    const auto& models = acemodel_catalog_provider().models;
    const auto it = std::find_if(models.begin(), models.end(), [&](const ModelEntry& model) {
        return lower_ascii(model.id) == normalized;
    });
    return it == models.end() ? nullptr : &*it;
}

bool is_acemodel_base_url(const std::string& base_url) {
    const auto& canonical = acemodel_catalog_provider().base_url;
    return canonical.has_value() &&
           normalize_base_url(base_url) == normalize_base_url(*canonical);
}

} // namespace acecode
