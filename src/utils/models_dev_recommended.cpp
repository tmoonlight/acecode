#include "models_dev_recommended.hpp"

#include "utf8_path.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace acecode {
namespace {

bool read_required_string(const nlohmann::json& object,
                          const char* key,
                          std::string& output,
                          std::string& error) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string() || it->get<std::string>().empty()) {
        error = std::string("recommended_models missing required string '") + key + "'";
        return false;
    }
    output = it->get<std::string>();
    return true;
}

bool read_required_positive_int(const nlohmann::json& object,
                                const char* key,
                                int& output,
                                std::string& error) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_integer()) {
        error = std::string("recommended_models missing required integer '") + key + "'";
        return false;
    }
    const long long value = it->get<long long>();
    if (value <= 0 || value > static_cast<long long>((std::numeric_limits<int>::max)())) {
        error = std::string("recommended_models field '") + key + "' must be positive";
        return false;
    }
    output = static_cast<int>(value);
    return true;
}

bool parse_string_array(const nlohmann::json& object,
                        const char* key,
                        std::vector<std::string>& output,
                        std::string& error) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_array()) {
        error = std::string("recommended_models missing required array '") + key + "'";
        return false;
    }
    std::set<std::string> unique;
    for (const auto& item : *it) {
        if (!item.is_string() || item.get<std::string>().empty()) {
            error = std::string("recommended_models field '") + key + "' has invalid value";
            return false;
        }
        std::string value = item.get<std::string>();
        if (!unique.insert(value).second) {
            error = std::string("recommended_models field '") + key + "' has duplicate value";
            return false;
        }
        output.push_back(std::move(value));
    }
    return true;
}

bool parse_reasoning(const nlohmann::json& value,
                     RecommendedReasoning& output,
                     std::string& error) {
    if (!value.is_object()) {
        error = "recommended_models reasoning must be an object";
        return false;
    }
    for (const char* key : {"supported", "mandatory", "default_enabled",
                            "supports_max_tokens"}) {
        const auto it = value.find(key);
        if (it == value.end() || !it->is_boolean()) {
            error = std::string("recommended_models reasoning missing boolean '") + key + "'";
            return false;
        }
    }
    output.supported = value["supported"].get<bool>();
    output.mandatory = value["mandatory"].get<bool>();
    output.default_enabled = value["default_enabled"].get<bool>();
    output.supports_max_tokens = value["supports_max_tokens"].get<bool>();
    if (!parse_string_array(value, "supported_efforts", output.supported_efforts, error)) {
        return false;
    }
    if (const auto it = value.find("default_effort"); it != value.end()) {
        if (!it->is_string() || it->get<std::string>().empty()) {
            error = "recommended_models reasoning.default_effort must be a non-empty string";
            return false;
        }
        output.default_effort = it->get<std::string>();
    }
    if (!output.supported &&
        (output.mandatory || output.default_enabled || output.default_effort.has_value() ||
         !output.supported_efforts.empty() || output.supports_max_tokens)) {
        error = "recommended_models reasoning options require supported=true";
        return false;
    }
    if (output.mandatory && !output.default_enabled) {
        error = "recommended_models mandatory reasoning must be enabled by default";
        return false;
    }
    if (output.default_effort.has_value() &&
        std::find(output.supported_efforts.begin(), output.supported_efforts.end(),
                  *output.default_effort) == output.supported_efforts.end()) {
        error = "recommended_models reasoning.default_effort is not supported";
        return false;
    }
    return true;
}

std::vector<std::string> capabilities_from_catalog(const ModelEntry& model) {
    std::vector<std::string> capabilities;
    if (model.attachment) capabilities.push_back("vision");
    if (model.tool_call) capabilities.push_back("tool_use");
    if (model.reasoning) capabilities.push_back("reasoning");
    return capabilities;
}

} // namespace

std::optional<RecommendedModelsManifest> parse_recommended_models_manifest(
    const nlohmann::json& value,
    std::string& error) {
    if (!value.is_object()) {
        error = "recommended_models root must be an object";
        return std::nullopt;
    }
    RecommendedModelsManifest manifest;
    const auto schema = value.find("schema_version");
    if (schema == value.end() || !schema->is_number_integer() ||
        schema->get<int>() != 1) {
        error = "recommended_models schema_version must be 1";
        return std::nullopt;
    }
    manifest.schema_version = 1;
    if (!read_required_string(value, "selected_at", manifest.selected_at, error) ||
        !read_required_string(value, "source_url", manifest.source_url, error) ||
        !read_required_string(value, "provider_id", manifest.provider_id, error)) {
        return std::nullopt;
    }
    const auto models = value.find("models");
    if (models == value.end() || !models->is_array() || models->size() != 5) {
        error = "recommended_models must contain exactly five models";
        return std::nullopt;
    }

    std::set<std::string> unique_ids;
    manifest.models.reserve(models->size());
    for (std::size_t index = 0; index < models->size(); ++index) {
        const auto& item = (*models)[index];
        if (!item.is_object()) {
            error = "recommended_models.models[" + std::to_string(index) + "] must be an object";
            return std::nullopt;
        }
        RecommendedModel model;
        model.provider_id = manifest.provider_id;
        if (!read_required_string(item, "model_id", model.model_id, error) ||
            !read_required_string(item, "name", model.name, error) ||
            !read_required_positive_int(item, "context_window", model.context_window, error) ||
            !read_required_positive_int(item, "max_output_tokens", model.max_output_tokens, error) ||
            !parse_string_array(item, "capabilities", model.capabilities, error)) {
            return std::nullopt;
        }
        if (!unique_ids.insert(model.model_id).second) {
            error = "recommended_models has duplicate model_id '" + model.model_id + "'";
            return std::nullopt;
        }
        const auto reasoning = item.find("reasoning");
        if (reasoning == item.end() || !parse_reasoning(*reasoning, model.reasoning, error)) {
            return std::nullopt;
        }
        const auto warning = item.find("privacy_warning");
        if (warning == item.end() || !warning->is_string()) {
            error = "recommended_models privacy_warning must be a string";
            return std::nullopt;
        }
        model.privacy_warning = warning->get<std::string>();
        manifest.models.push_back(std::move(model));
    }
    return manifest;
}

std::optional<RecommendedModelsManifest> load_recommended_models_manifest(
    const std::filesystem::path& models_dev_dir,
    std::string& error) {
    const auto path = models_dev_dir / "recommended_models.json";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        error = "recommended_models.json is missing from " + path_to_utf8(models_dev_dir);
        return std::nullopt;
    }
    std::ifstream input(path);
    if (!input.is_open()) {
        error = "recommended_models.json could not be opened";
        return std::nullopt;
    }
    try {
        nlohmann::json value;
        input >> value;
        return parse_recommended_models_manifest(value, error);
    } catch (const std::exception& e) {
        error = std::string("recommended_models.json is invalid JSON: ") + e.what();
        return std::nullopt;
    }
}

RecommendedModelsManifest supplement_recommended_models(
    RecommendedModelsManifest manifest,
    const std::vector<ProviderEntry>& catalog) {
    const auto provider = std::find_if(
        catalog.begin(), catalog.end(), [&](const ProviderEntry& value) {
            return value.id == manifest.provider_id;
        });
    if (provider == catalog.end()) return manifest;

    for (auto& recommendation : manifest.models) {
        const auto model = std::find_if(
            provider->models.begin(), provider->models.end(),
            [&](const ModelEntry& value) { return value.id == recommendation.model_id; });
        if (model == provider->models.end()) continue;
        recommendation.name = model->name;
        if (model->context.has_value() && *model->context > 0) {
            recommendation.context_window = *model->context;
        }
        if (model->max_output.has_value() && *model->max_output > 0) {
            recommendation.max_output_tokens = *model->max_output;
        }
        recommendation.capabilities = capabilities_from_catalog(*model);
        recommendation.reasoning.supported = model->reasoning;
        recommendation.reasoning.mandatory =
            model->reasoning && !model->reasoning_can_disable;
        recommendation.reasoning.default_enabled = model->reasoning;
        recommendation.reasoning.supported_efforts = model->reasoning_efforts;
        recommendation.reasoning.supports_max_tokens =
            model->reasoning_supports_max_tokens;
        if (recommendation.reasoning.default_effort.has_value() &&
            std::find(recommendation.reasoning.supported_efforts.begin(),
                      recommendation.reasoning.supported_efforts.end(),
                      *recommendation.reasoning.default_effort) ==
                recommendation.reasoning.supported_efforts.end()) {
            recommendation.reasoning.default_effort.reset();
        }
        recommendation.deprecated = model->deprecated;
    }
    return manifest;
}

nlohmann::json recommended_reasoning_to_json(
    const RecommendedReasoning& reasoning) {
    nlohmann::json value{
        {"supported", reasoning.supported},
        {"mandatory", reasoning.mandatory},
        {"default_enabled", reasoning.default_enabled},
        {"supported_efforts", reasoning.supported_efforts},
        {"supports_max_tokens", reasoning.supports_max_tokens},
    };
    if (reasoning.default_effort.has_value()) {
        value["default_effort"] = *reasoning.default_effort;
    }
    return value;
}

nlohmann::json recommended_model_to_json(const RecommendedModel& model) {
    return nlohmann::json{
        {"provider_id", model.provider_id},
        {"model_id", model.model_id},
        {"name", model.name},
        {"context_window", model.context_window},
        {"max_output_tokens", model.max_output_tokens},
        {"capabilities", model.capabilities},
        {"reasoning", recommended_reasoning_to_json(model.reasoning)},
        {"deprecated", model.deprecated},
        {"privacy_warning", model.privacy_warning},
    };
}

} // namespace acecode
