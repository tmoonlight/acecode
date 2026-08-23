#include "builtin_model_catalog.hpp"

#include <algorithm>
#include <cctype>

namespace acecode {
namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

ModelEntry builtin_model(const std::string& id, const std::string& name) {
    ModelEntry model;
    model.id = id;
    model.name = name;
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

} // namespace acecode
