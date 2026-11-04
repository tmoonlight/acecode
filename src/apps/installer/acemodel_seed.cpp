#include "acemodel_seed.hpp"

#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace acecode::installer {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

constexpr std::array<const char*, 3> kProfiles{"moonlight", "starrylight", "aurora"};
constexpr int kContextWindow = 250000;

std::string trim(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

bool blank_string_field(const Json& object, const char* name) {
    const auto it = object.find(name);
    return it == object.end() || !it->is_string() || trim(it->get<std::string>()).empty();
}

Json default_capabilities() {
    return Json::array({"vision", "tool_use"});
}

Json new_profile(const std::string& name, const std::string& key) {
    Json profile = Json::object();
    profile["name"] = name;
    profile["provider"] = "openai";
    profile["model"] = name;
    profile["base_url"] = kAceModelBaseUrl;
    profile["api_key"] = key;
    profile["models_dev_provider_id"] = "acemodel";
    profile["endpoint_mode"] = "base_url";
    profile["api_protocol"] = "responses";
    profile["context_window"] = kContextWindow;
    profile["capabilities"] = default_capabilities();
    profile["capabilities_source"] = "catalog";
    return profile;
}

bool matches(const Json& entry, const std::string& name) {
    const auto field = [&entry](const char* key) {
        const auto it = entry.find(key);
        return it != entry.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    return field("name") == name || field("model") == name;
}

void upsert(Json& models, const std::string& name, const std::string& key) {
    for (auto& entry : models) {
        if (!entry.is_object() || !matches(entry, name)) continue;
        entry["name"] = name;
        entry["provider"] = "openai";
        entry["model"] = name;
        entry["base_url"] = kAceModelBaseUrl;
        entry["api_key"] = key;
        entry["models_dev_provider_id"] = "acemodel";
        entry["context_window"] = kContextWindow;
        if (blank_string_field(entry, "api_protocol")) entry["api_protocol"] = "responses";
        if (blank_string_field(entry, "endpoint_mode")) entry["endpoint_mode"] = "base_url";
        const auto source = entry.find("capabilities_source");
        const bool manual = source != entry.end() && source->is_string() &&
                            source->get<std::string>() == "manual";
        if (!manual) {
            entry["capabilities"] = default_capabilities();
            entry["capabilities_source"] = "catalog";
        }
        return;
    }
    models.push_back(new_profile(name, key));
}

}  // namespace

std::optional<std::string> merge_acemodel_profiles(const std::string& config_text,
                                                   const std::string& api_key,
                                                   std::string* error) {
    const std::string key = trim(api_key);
    if (key.empty()) {
        if (error) *error = "ACEModel key is empty";
        return std::nullopt;
    }
    Json config = Json::object();
    if (!trim(config_text).empty()) {
        config = Json::parse(config_text, nullptr, false);
        if (config.is_discarded() || !config.is_object()) {
            if (error) *error = "config.json root must be a JSON object";
            return std::nullopt;
        }
    }
    Json models = Json::array();
    const auto existing = config.find("saved_models");
    if (existing != config.end()) {
        if (existing->is_array()) {
            models = *existing;
        } else if (existing->is_object()) {
            models.push_back(*existing);
        }
    }
    for (const char* name : kProfiles) upsert(models, name, key);
    config["saved_models"] = models;
    if (blank_string_field(config, "default_model_name")) {
        config["default_model_name"] = kAceModelDefaultModel;
    }
    return config.dump(2) + "\n";
}

bool seed_acemodel_config_file(const fs::path& config_path,
                               const std::string& api_key,
                               std::string* error) {
    std::string current;
    {
        std::ifstream in(config_path, std::ios::binary);
        if (in) {
            std::ostringstream text;
            text << in.rdbuf();
            current = text.str();
        }
    }
    if (current.size() >= 3 && static_cast<unsigned char>(current[0]) == 0xEF &&
        static_cast<unsigned char>(current[1]) == 0xBB &&
        static_cast<unsigned char>(current[2]) == 0xBF) {
        current.erase(0, 3);
    }
    const auto merged = merge_acemodel_profiles(current, api_key, error);
    if (!merged) return false;

    std::error_code ec;
    fs::create_directories(config_path.parent_path(), ec);
    const fs::path temp = config_path.parent_path() /
                          (config_path.filename().string() + ".setup-tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << *merged;
        if (!out) {
            if (error) *error = "failed to write " + path_to_utf8(temp);
            return false;
        }
    }
    fs::rename(temp, config_path, ec);
    if (ec) {
        fs::remove(temp, ec);
        if (error) *error = "failed to replace " + path_to_utf8(config_path);
        return false;
    }
    return true;
}

}  // namespace acecode::installer
