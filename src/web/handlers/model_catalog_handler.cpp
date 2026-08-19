#include "model_catalog_handler.hpp"

#include "../../config/saved_models.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::web {
namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string source_name(RegistrySource::Kind kind) {
    switch (kind) {
        case RegistrySource::Kind::Bundled: return "bundled";
        case RegistrySource::Kind::UserOverride: return "user_override";
        case RegistrySource::Kind::Network: return "network";
        case RegistrySource::Kind::Empty: return "empty";
    }
    return "empty";
}

std::string freshness_name(RegistrySource::Kind kind) {
    switch (kind) {
        case RegistrySource::Kind::Network: return "live";
        case RegistrySource::Kind::Bundled: return "bundled";
        case RegistrySource::Kind::UserOverride: return "custom";
        case RegistrySource::Kind::Empty: return "unavailable";
    }
    return "unavailable";
}

std::string first_env(const ProviderEntry* provider) {
    return provider && !provider->env.empty() ? provider->env.front() : "";
}

const ProviderEntry* by_id(const std::vector<ProviderEntry>& providers,
                           const std::string& id) {
    const std::string needle = lower_ascii(id);
    for (const auto& provider : providers) {
        if (lower_ascii(provider.id) == needle) return &provider;
    }
    return nullptr;
}

bool no_auth_openai_provider(const ProviderEntry& provider) {
    ModelProfile profile;
    profile.provider = "openai";
    profile.base_url = provider.base_url.value_or("");
    profile.models_dev_provider_id = provider.id;
    return model_profile_allows_no_api_key(profile);
}

constexpr const char* kAceModelProviderId = "acemodel";
constexpr const char* kAceModelBaseUrl = "https://ge.bigjuan.xyz/aceapi/v1";

ModelEntry builtin_catalog_model(const std::string& id, const std::string& name) {
    ModelEntry model;
    model.id = id;
    model.name = name;
    model.tool_call = true;
    return model;
}

const std::vector<ModelEntry>& acemodel_builtin_models() {
    static const std::vector<ModelEntry> models{
        builtin_catalog_model("moonlight", "Moonlight"),
        builtin_catalog_model("starrylight", "Starrylight"),
    };
    return models;
}

nlohmann::json provider_to_json(const std::string& id,
                                const std::string& name,
                                const std::string& runtime_provider,
                                const std::string& base_url,
                                const std::string& doc,
                                const std::string& auth_mode,
                                bool endpoint_editable,
                                const std::string& model_input,
                                const std::string& api_key_env,
                                const std::optional<std::string>& models_dev_provider_id,
                                const std::string& group,
                                const std::vector<std::string>& endpoint_modes) {
    return nlohmann::json{
        {"id", id},
        {"name", name},
        {"runtime_provider", runtime_provider},
        {"base_url", base_url},
        {"doc", doc},
        {"auth_mode", auth_mode},
        {"endpoint_editable", endpoint_editable},
        {"model_input", model_input},
        {"api_key_env", api_key_env},
        {"models_dev_provider_id", models_dev_provider_id.has_value()
            ? nlohmann::json(*models_dev_provider_id)
            : nlohmann::json(nullptr)},
        {"group", group},
        {"endpoint_modes", endpoint_modes},
    };
}

std::vector<nlohmann::json> provider_descriptors(
    const std::vector<ProviderEntry>& providers) {
    std::vector<nlohmann::json> result;
    const ProviderEntry* anthropic = by_id(providers, "anthropic");
    const ProviderEntry* openai = by_id(providers, "openai");
    const ProviderEntry* xai = by_id(providers, "xai");
    result.push_back(provider_to_json(
        "anthropic",
        anthropic ? anthropic->name : "Anthropic",
        "anthropic",
        "https://api.anthropic.com/v1",
        anthropic && anthropic->doc ? *anthropic->doc : "",
        "required",
        false,
        "catalog",
        first_env(anthropic),
        std::string("anthropic"),
        "native",
        {"base_url"}));
    result.push_back(provider_to_json(
        "copilot", "GitHub Copilot", "copilot", "",
        "https://docs.github.com/en/copilot", "managed", false, "catalog", "",
        std::nullopt, "native", {}));
    result.push_back(provider_to_json(
        "grok", "Grok Coding Plan", "grok", "",
        xai && xai->doc ? *xai->doc : "https://docs.x.ai",
        "managed", false, "catalog", "", std::string("xai"), "native", {}));
    result.push_back(provider_to_json(
        kAceModelProviderId,
        "ACEModel",
        "openai",
        kAceModelBaseUrl,
        "",
        "required",
        false,
        "catalog",
        "ACEMODEL_API_KEY",
        std::string(kAceModelProviderId),
        "custom",
        {"base_url"}));
    result.push_back(provider_to_json(
        "custom-openai", "Custom OpenAI-compatible API", "openai", "", "",
        "required", true, "manual", "", std::nullopt, "custom",
        {"base_url", "full_url"}));
    result.push_back(provider_to_json(
        "openai",
        openai ? openai->name : "OpenAI",
        "openai",
        "https://api.openai.com/v1",
        openai && openai->doc ? *openai->doc : "",
        "required",
        false,
        "catalog",
        first_env(openai),
        std::string("openai"),
        "native",
        {"base_url"}));
    if (xai != nullptr) {
        result.push_back(provider_to_json(
            "xai",
            xai->name,
            "openai",
            "https://api.x.ai/v1",
            xai->doc.value_or(""),
            "required",
            false,
            "catalog",
            first_env(xai),
            std::string("xai"),
            "catalog",
            {"base_url"}));
    }

    for (const auto& provider : providers) {
        if (!provider.openai_compatible || !provider.base_url.has_value()) continue;
        const std::string id = lower_ascii(provider.id);
        if (id == "anthropic" || id == "github-copilot" || id == "copilot" ||
            id == "openai" || id == "xai" || id == kAceModelProviderId) {
            continue;
        }
        result.push_back(provider_to_json(
            provider.id,
            provider.name,
            "openai",
            *provider.base_url,
            provider.doc.value_or(""),
            no_auth_openai_provider(provider) ? "none" : "required",
            false,
            "catalog",
            first_env(&provider),
            provider.id,
            no_auth_openai_provider(provider) ? "local" : "catalog",
            {"base_url"}));
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        const std::string left_name =
            lower_ascii(left["name"].template get<std::string>());
        const std::string right_name =
            lower_ascii(right["name"].template get<std::string>());
        if (left_name != right_name) return left_name < right_name;
        return left["id"].template get<std::string>() <
               right["id"].template get<std::string>();
    });
    return result;
}

std::vector<std::string> model_capabilities(const ModelEntry& model) {
    std::vector<std::string> result;
    if (model.attachment) result.push_back("vision");
    if (model.tool_call) result.push_back("tool_use");
    if (model.reasoning) result.push_back("reasoning");
    return result;
}

nlohmann::json model_to_json(const ModelEntry& model) {
    nlohmann::json result{
        {"id", model.id},
        {"name", model.name},
        {"context_window", model.context.has_value() && *model.context > 0
            ? nlohmann::json(*model.context)
            : nlohmann::json(nullptr)},
        {"max_output_tokens", model.max_output.has_value() && *model.max_output > 0
            ? nlohmann::json(*model.max_output)
            : nlohmann::json(nullptr)},
        {"capabilities", model_capabilities(model)},
        {"reasoning", {
            {"supported", model.reasoning},
            {"mandatory", model.reasoning && !model.reasoning_can_disable},
            {"default_enabled", model.reasoning},
            {"supported_efforts", model.reasoning_efforts},
            {"supports_max_tokens", model.reasoning_supports_max_tokens},
        }},
        {"deprecated", model.deprecated},
        {"input_modalities", model.input_modalities},
        {"output_modalities", model.output_modalities},
        {"knowledge_cutoff", model.knowledge_cutoff.has_value()
            ? nlohmann::json(*model.knowledge_cutoff)
            : nlohmann::json(nullptr)},
        {"pricing", {
            {"input", model.cost_input.has_value()
                ? nlohmann::json(*model.cost_input)
                : nlohmann::json(nullptr)},
            {"output", model.cost_output.has_value()
                ? nlohmann::json(*model.cost_output)
                : nlohmann::json(nullptr)},
        }},
    };
    return result;
}

const ProviderEntry* query_provider(const std::vector<ProviderEntry>& providers,
                                    const std::string& id) {
    const std::string normalized = lower_ascii(id);
    if (normalized == "custom-openai" || normalized == kAceModelProviderId) {
        return nullptr;
    }
    if (normalized == "copilot") return by_id(providers, "github-copilot");
    if (normalized == "grok") return by_id(providers, "xai");
    return by_id(providers, normalized);
}

} // namespace

nlohmann::json catalog_metadata_to_json(const RegistrySource& source,
                                        unsigned long long version) {
    std::string updated_at;
    if (source.manifest.has_value()) {
        const auto it = source.manifest->find("generated_at");
        if (it != source.manifest->end() && it->is_string()) {
            updated_at = it->get<std::string>();
        }
    }
    return nlohmann::json{
        {"source", source_name(source.kind)},
        {"version", version},
        {"updated_at", updated_at},
        {"freshness", freshness_name(source.kind)},
    };
}

nlohmann::json model_catalog_summary_to_json(
    const std::vector<ProviderEntry>& providers,
    const RegistrySource& source,
    unsigned long long version) {
    nlohmann::json provider_json = nlohmann::json::array();
    for (auto& provider : provider_descriptors(providers)) {
        provider_json.push_back(std::move(provider));
    }
    return nlohmann::json{
        {"catalog", catalog_metadata_to_json(source, version)},
        {"providers", std::move(provider_json)},
    };
}

std::optional<nlohmann::json> query_model_catalog_to_json(
    const std::vector<ProviderEntry>& providers,
    const std::string& provider_id,
    const std::string& query,
    int limit) {
    const std::string normalized_provider = lower_ascii(provider_id);
    if (normalized_provider == "custom-openai") {
        return nlohmann::json{
            {"provider_id", provider_id},
            {"models", nlohmann::json::array()},
            {"limit", std::clamp(limit, 1, kMaxModelCatalogQueryLimit)},
        };
    }

    const int effective_limit = limit <= 0
        ? kDefaultModelCatalogQueryLimit
        : std::min(limit, kMaxModelCatalogQueryLimit);
    const std::string needle = lower_ascii(query);
    std::vector<const ModelEntry*> matches;
    const std::vector<ModelEntry>* model_source = nullptr;
    if (normalized_provider == kAceModelProviderId) {
        model_source = &acemodel_builtin_models();
    } else {
        const ProviderEntry* provider = query_provider(providers, provider_id);
        if (!provider) return std::nullopt;
        model_source = &provider->models;
    }

    for (const auto& model : *model_source) {
        const std::string id = lower_ascii(model.id);
        const std::string name = lower_ascii(model.name);
        if (needle.empty() || id.find(needle) != std::string::npos ||
            name.find(needle) != std::string::npos) {
            matches.push_back(&model);
        }
    }
    std::stable_sort(matches.begin(), matches.end(), [&](const ModelEntry* left,
                                                         const ModelEntry* right) {
        const bool left_exact = lower_ascii(left->id) == needle;
        const bool right_exact = lower_ascii(right->id) == needle;
        if (left_exact != right_exact) return left_exact;
        return lower_ascii(left->id) < lower_ascii(right->id);
    });

    nlohmann::json model_json = nlohmann::json::array();
    for (std::size_t index = 0;
         index < matches.size() && index < static_cast<std::size_t>(effective_limit);
         ++index) {
        model_json.push_back(model_to_json(*matches[index]));
    }
    return nlohmann::json{
        {"provider_id", provider_id},
        {"models", std::move(model_json)},
        {"limit", effective_limit},
    };
}

} // namespace acecode::web
