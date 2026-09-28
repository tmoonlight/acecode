#include "tool_rewrites.hpp"

#include "utils/atomic_file.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace acecode::tool_rewrites {

namespace {

void set_error(std::string* error, const std::string& value) {
    if (error) *error = value;
}

std::string trim_ascii(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

} // namespace

std::string settings_path(const std::string& data_dir) {
    return path_to_utf8(path_from_utf8(data_dir) / kSettingsFileName);
}

ToolRewriteSettings default_settings() {
    ToolRewriteSettings settings;
    settings.enabled = false;
    settings.rewrites = default_model_tool_name_mappings();
    return settings;
}

bool parse_settings(const std::string& text,
                    ToolRewriteSettings& out,
                    std::string* error) {
    const auto doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        set_error(error, "tool-rewrites.json must be a JSON object");
        return false;
    }

    ToolRewriteSettings parsed;
    if (auto it = doc.find("enabled"); it != doc.end()) {
        if (!it->is_boolean()) {
            set_error(error, "'enabled' must be a boolean");
            return false;
        }
        parsed.enabled = it->get<bool>();
    }
    if (auto it = doc.find("rewrites"); it != doc.end()) {
        if (!it->is_object()) {
            set_error(error, "'rewrites' must be an object of native → public names");
            return false;
        }
        for (auto entry = it->begin(); entry != it->end(); ++entry) {
            if (!entry.value().is_string()) {
                set_error(error, "rewrite target for '" + entry.key() +
                                     "' must be a string");
                return false;
            }
            const std::string native = trim_ascii(entry.key());
            const std::string target = trim_ascii(entry.value().get<std::string>());
            if (native.empty()) {
                set_error(error, "rewrite keys must not be empty");
                return false;
            }
            // 空值 / 与原名相同 = 不重写,直接丢弃,文件里留着也无害。
            if (target.empty() || target == native) continue;
            parsed.rewrites.push_back({native, target});
        }
    }

    out = std::move(parsed);
    if (error) error->clear();
    return true;
}

std::string serialize_settings(const ToolRewriteSettings& settings) {
    nlohmann::json rewrites = nlohmann::json::object();
    for (const auto& mapping : settings.rewrites) {
        rewrites[mapping.native_name] = mapping.public_name;
    }
    const nlohmann::json doc{
        {"version", kSettingsVersion},
        {"enabled", settings.enabled},
        {"rewrites", rewrites},
    };
    return doc.dump(2) + "\n";
}

bool validate_settings(const ToolRewriteSettings& settings, std::string* error) {
    return validate_model_tool_name_mappings(settings.rewrites, error);
}

bool validate_settings_against_tools(
    const ToolRewriteSettings& settings,
    const std::vector<std::string>& registered_tool_names,
    std::string* error) {
    if (!validate_settings(settings, error)) return false;
    for (const auto& mapping : settings.rewrites) {
        const bool collides = std::find(registered_tool_names.begin(),
                                        registered_tool_names.end(),
                                        mapping.public_name) !=
                              registered_tool_names.end();
        if (collides) {
            set_error(error, "rewrite target '" + mapping.public_name +
                                 "' for '" + mapping.native_name +
                                 "' is already the name of another tool");
            return false;
        }
    }
    if (error) error->clear();
    return true;
}

ToolRewriteSettings load_settings(const std::string& path, std::string* error) {
    if (error) error->clear();
    std::error_code ec;
    const auto fs_path = path_from_utf8(path);
    if (!std::filesystem::exists(fs_path, ec) || ec) return default_settings();

    std::ifstream in(fs_path, std::ios::binary);
    if (!in.is_open()) {
        set_error(error, "cannot open " + path);
        return default_settings();
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());

    ToolRewriteSettings parsed;
    std::string parse_error;
    if (!parse_settings(text, parsed, &parse_error)) {
        set_error(error, path + ": " + parse_error);
        return default_settings();
    }
    std::string validation_error;
    if (!validate_settings(parsed, &validation_error)) {
        set_error(error, path + ": " + validation_error);
        return default_settings();
    }
    return parsed;
}

bool save_settings(const std::string& path,
                   const ToolRewriteSettings& settings,
                   std::string* error) {
    if (!validate_settings(settings, error)) return false;
    if (!atomic_write_file(path, serialize_settings(settings))) {
        set_error(error, "cannot write " + path);
        return false;
    }
    if (error) error->clear();
    return true;
}

ToolProtocolNameMappings effective_mappings(const ToolRewriteSettings& settings) {
    if (!settings.enabled) return {};
    return settings.rewrites;
}

bool apply_to_process(const ToolRewriteSettings& settings, std::string* error) {
    return set_model_tool_name_mappings(effective_mappings(settings), error);
}

ToolRewriteSettings load_and_apply(const std::string& data_dir) {
    const std::string path = settings_path(data_dir);
    std::string error;
    ToolRewriteSettings settings = load_settings(path, &error);
    if (!error.empty()) {
        LOG_WARN("Ignoring tool rewrites: " + error);
    }
    std::string apply_error;
    if (!apply_to_process(settings, &apply_error)) {
        LOG_WARN("Ignoring tool rewrites: " + apply_error);
        settings = default_settings();
        apply_to_process(settings);
    } else if (settings.enabled && !settings.rewrites.empty()) {
        std::ostringstream summary;
        for (const auto& mapping : settings.rewrites) {
            if (summary.tellp() > 0) summary << ", ";
            summary << mapping.native_name << " -> " << mapping.public_name;
        }
        LOG_INFO("Tool rewrites active: " + summary.str());
    }
    return settings;
}

} // namespace acecode::tool_rewrites
