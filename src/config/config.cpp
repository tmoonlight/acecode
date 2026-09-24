#include "config.hpp"

#include "permissions.hpp"
#include "config_recovery.hpp"
#include "config_mutation.hpp"
#include "mcp_config.hpp"
#include "model_provider_registry.hpp"
#include "request_headers.hpp"
#include "../themes/theme_id.hpp"
#include "../utils/constants.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/logger.hpp"
#include "../utils/http_url_validation.hpp"
#include "../utils/paths.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace acecode {

namespace {

std::atomic<bool> g_acecode_home_created_by_process{false};

// Call only for integer JSON values. Compare before narrowing so large signed
// or unsigned values clamp to the intended boundary instead of wrapping.
int clamp_config_integer(const nlohmann::json& value, int minimum, int maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value.get<int>();
}

class ConfigLoadFailure final : public std::runtime_error {
public:
    ConfigLoadFailure(std::string category, std::string summary)
        : std::runtime_error(std::move(summary)),
          category_(std::move(category)) {}

    const std::string& category() const noexcept { return category_; }

private:
    std::string category_;
};

std::string trim_ascii_copy(const std::string& s) {
    size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) {
        ++first;
    }
    size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) {
        --last;
    }
    return s.substr(first, last - first);
}

std::string normalized_web_bind(std::string value) {
    value = trim_ascii_copy(value);
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

bool web_bind_is_loopback(const std::string& raw_bind) {
    const std::string bind = normalized_web_bind(raw_bind);
    if (bind == "localhost" || bind == "::1") return true;
    if (bind.rfind("127.", 0) == 0) return true;
    constexpr const char* kMappedPrefix = "::ffff:";
    if (bind.rfind(kMappedPrefix, 0) == 0) {
        return bind.substr(std::char_traits<char>::length(kMappedPrefix))
            .rfind("127.", 0) == 0;
    }
    return false;
}

bool is_one_of(const std::string& value, std::initializer_list<const char*> allowed) {
    for (const char* item : allowed) {
        if (value == item) return true;
    }
    return false;
}

std::string normalize_permission_mode_name(std::string value) {
    // 别名(accept-edits / acceptEdits → auto)集中在 PermissionManager 维护;
    // 老配置里的 accept-edits 读进来即归一成 auto,下次保存写 auto。
    if (auto parsed = PermissionManager::parse_mode_name(value)) {
        return PermissionManager::mode_name(*parsed);
    }
    if (!value.empty()) {
        LOG_WARN("[config] default_permission_mode='" + value +
                 "' invalid; falling back to 'default'");
    }
    return "default";
}

std::optional<int> parse_positive_int(const std::string& value) {
    const std::string trimmed = trim_ascii_copy(value);
    if (trimmed.empty()) return std::nullopt;
    try {
        std::size_t pos = 0;
        long long parsed = std::stoll(trimmed, &pos, 10);
        if (pos != trimmed.size() ||
            parsed <= 0 ||
            parsed > std::numeric_limits<int>::max()) {
            return std::nullopt;
        }
        return static_cast<int>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

[[noreturn]] void fatal_config_value(const std::string& message) {
    throw ConfigLoadFailure("semantic_validation", message);
}

[[noreturn]] void fatal_runtime_config_value(const std::string& message) {
    std::cerr << "[config] fatal: " << message << std::endl;
    LOG_ERROR("[config] " + message);
    std::exit(1);
}

std::string legacy_model_profile_name(const AppConfig& cfg) {
    if (cfg.provider == "openai") {
        if (cfg.openai.models_dev_provider_id.has_value() &&
            !cfg.openai.models_dev_provider_id->empty()) {
            return *cfg.openai.models_dev_provider_id;
        }
        return "openai";
    }
    if (cfg.provider == "copilot") return "copilot";
    if (cfg.provider == "grok") return "grok";
    return "";
}

nlohmann::json connector_hook_to_json(const ConnectorHookConfig& hook) {
    return {
        {"command", hook.command},
        {"args", hook.args},
        {"timeout_ms", hook.timeout_ms},
    };
}

bool parse_connector_hook(const nlohmann::json& item,
                          ConnectorHookConfig& out,
                          std::string& error_msg) {
    if (!item.is_object() || !item.contains("command") || !item["command"].is_string()) {
        error_msg = "must be an object with string command";
        return false;
    }
    out.command = item["command"].get<std::string>();
    if (out.command.empty()) {
        error_msg = "command must not be empty";
        return false;
    }
    out.args.clear();
    if (item.contains("args")) {
        if (!item["args"].is_array()) {
            error_msg = "args must be an array of strings";
            return false;
        }
        for (const auto& arg : item["args"]) {
            if (!arg.is_string()) {
                error_msg = "args must be an array of strings";
                return false;
            }
            out.args.push_back(arg.get<std::string>());
        }
    }
    if (item.contains("timeout_ms")) {
        if (!item["timeout_ms"].is_number_integer()) {
            error_msg = "timeout_ms must be an integer";
            return false;
        }
        const int timeout = item["timeout_ms"].get<int>();
        if (timeout > 0) out.timeout_ms = timeout;
    }
    return true;
}

} // namespace

std::string normalize_upgrade_base_url(std::string raw) {
    raw = trim_ascii_copy(raw);
    if (!raw.empty() && raw.back() != '/') {
        raw.push_back('/');
    }
    return raw;
}

bool is_valid_upgrade_base_url(const std::string& raw) {
    const std::string url = normalize_upgrade_base_url(raw);
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

bool is_valid_ui_locale(const std::string& locale) {
    return locale == "auto" || locale == "zh-CN" || locale == "en-US";
}

bool is_valid_web_ui_theme(const std::string& theme) {
    return theme == "system" || theme == "light" || theme == "dark";
}

bool is_valid_web_ui_color_theme(const std::string& color_theme) {
    return color_theme == "blue" || color_theme == "orange" || themes::is_downloadable_theme(color_theme) ||
        themes::is_local_theme(color_theme);
}

bool is_valid_web_ui_font_size(const std::string& font_size) {
    return font_size == "small" || font_size == "medium" ||
           font_size == "large";
}

nlohmann::json connectors_to_json(const std::vector<ConnectorConfig>& connectors) {
    nlohmann::json items = nlohmann::json::array();
    for (const auto& connector : connectors) {
        nlohmann::json item = {
            {"id", connector.id},
            {"name", connector.name},
            {"description", connector.description},
            {"enabled", connector.enabled},
        };
        nlohmann::json hooks = nlohmann::json::object();
        if (connector.on_enable) {
            hooks["on_enable"] = connector_hook_to_json(*connector.on_enable);
        }
        if (connector.on_auth_error) {
            hooks["on_auth_error"] = connector_hook_to_json(*connector.on_auth_error);
        }
        if (connector.on_startup) {
            hooks["on_startup"] = connector_hook_to_json(*connector.on_startup);
        }
        if (!hooks.empty()) item["hooks"] = std::move(hooks);
        if (!connector.auth_error_base_url_prefix.empty()) {
            item["auth_error_scope"] = {
                {"base_url_prefix", connector.auth_error_base_url_prefix},
            };
        }
        items.push_back(std::move(item));
    }
    return items;
}

bool parse_connectors_json(const nlohmann::json& value,
                           std::vector<ConnectorConfig>& out,
                           std::string* error) {
    if (!value.is_array()) {
        if (error) *error = "connectors must be an array";
        return false;
    }

    std::vector<ConnectorConfig> parsed;
    std::set<std::string> seen_ids;
    parsed.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto& item = value[i];
        auto fail = [&](const std::string& message) {
            if (error) {
                *error = "connectors[" + std::to_string(i) + "] " + message;
            }
            return false;
        };
        if (!item.is_object()) return fail("must be an object");
        if (!item.contains("id") || !item["id"].is_string()) {
            return fail("must contain string id");
        }
        if (!item.contains("name") || !item["name"].is_string()) {
            return fail("must contain string name");
        }
        if (!item.contains("description") || !item["description"].is_string()) {
            return fail("must contain string description");
        }
        if (!item.contains("enabled") || !item["enabled"].is_boolean()) {
            return fail("must contain boolean enabled");
        }

        ConnectorConfig connector;
        connector.id = trim_ascii_copy(item["id"].get<std::string>());
        connector.name = item["name"].get<std::string>();
        connector.description = item["description"].get<std::string>();
        connector.enabled = item["enabled"].get<bool>();
        if (item.contains("hooks")) {
            const auto& hooks = item["hooks"];
            if (!hooks.is_object()) return fail("hooks must be an object");
            std::string hook_error;
            if (hooks.contains("on_enable")) {
                ConnectorHookConfig hook;
                if (!parse_connector_hook(hooks["on_enable"], hook, hook_error)) {
                    return fail("hooks.on_enable " + hook_error);
                }
                connector.on_enable = std::move(hook);
            }
            if (hooks.contains("on_auth_error")) {
                ConnectorHookConfig hook;
                if (!parse_connector_hook(hooks["on_auth_error"], hook, hook_error)) {
                    return fail("hooks.on_auth_error " + hook_error);
                }
                connector.on_auth_error = std::move(hook);
            }
            if (hooks.contains("on_startup")) {
                ConnectorHookConfig hook;
                if (!parse_connector_hook(hooks["on_startup"], hook, hook_error)) {
                    return fail("hooks.on_startup " + hook_error);
                }
                connector.on_startup = std::move(hook);
            }
        }
        if (item.contains("auth_error_scope")) {
            const auto& scope = item["auth_error_scope"];
            if (!scope.is_object()) return fail("auth_error_scope must be an object");
            if (scope.contains("base_url_prefix")) {
                if (!scope["base_url_prefix"].is_string()) {
                    return fail("auth_error_scope.base_url_prefix must be a string");
                }
                connector.auth_error_base_url_prefix =
                    trim_ascii_copy(scope["base_url_prefix"].get<std::string>());
            }
        }
        if (connector.id.empty()) return fail("id must not be empty");
        if (connector.name.empty()) return fail("name must not be empty");
        if (!seen_ids.insert(connector.id).second) {
            return fail("id must be unique: " + connector.id);
        }
        parsed.push_back(std::move(connector));
    }

    out = std::move(parsed);
    return true;
}

std::vector<ConnectorConfig> startup_hook_connectors(
        const std::vector<ConnectorConfig>& connectors) {
    std::vector<ConnectorConfig> out;
    for (const auto& connector : connectors) {
        if (connector.enabled && connector.on_startup) out.push_back(connector);
    }
    return out;
}

void load_connectors_lenient(const nlohmann::json& value,
                             std::vector<ConnectorConfig>& out) {
    if (!value.is_array()) {
        LOG_WARN("[config] 'connectors' must be an array, ignoring");
        return;
    }

    std::set<std::string> seen_ids;
    std::vector<ConnectorConfig> parsed;
    parsed.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto& item = value[i];
        if (!item.is_object() ||
            !item.contains("id") || !item["id"].is_string() ||
            !item.contains("name") || !item["name"].is_string() ||
            !item.contains("description") || !item["description"].is_string()) {
            LOG_WARN("[config] connectors[" + std::to_string(i) +
                     "] missing required id/name/description strings, skipping");
            continue;
        }

        ConnectorConfig connector;
        connector.id = trim_ascii_copy(item["id"].get<std::string>());
        connector.name = item["name"].get<std::string>();
        connector.description = item["description"].get<std::string>();
        connector.enabled = item.contains("enabled") && item["enabled"].is_boolean()
            ? item["enabled"].get<bool>()
            : true;
        if (item.contains("hooks") && item["hooks"].is_object()) {
            const auto& hooks = item["hooks"];
            if (hooks.contains("on_enable")) {
                ConnectorHookConfig hook;
                std::string hook_error;
                if (parse_connector_hook(hooks["on_enable"], hook, hook_error)) {
                    connector.on_enable = std::move(hook);
                } else {
                    LOG_WARN("[config] connectors[" + std::to_string(i) +
                             "] hooks.on_enable " + hook_error + ", ignoring hook");
                }
            }
            if (hooks.contains("on_auth_error")) {
                ConnectorHookConfig hook;
                std::string hook_error;
                if (parse_connector_hook(hooks["on_auth_error"], hook, hook_error)) {
                    connector.on_auth_error = std::move(hook);
                } else {
                    LOG_WARN("[config] connectors[" + std::to_string(i) +
                             "] hooks.on_auth_error " + hook_error + ", ignoring hook");
                }
            }
            if (hooks.contains("on_startup")) {
                ConnectorHookConfig hook;
                std::string hook_error;
                if (parse_connector_hook(hooks["on_startup"], hook, hook_error)) {
                    connector.on_startup = std::move(hook);
                } else {
                    LOG_WARN("[config] connectors[" + std::to_string(i) +
                             "] hooks.on_startup " + hook_error + ", ignoring hook");
                }
            }
        } else if (item.contains("hooks")) {
            LOG_WARN("[config] connectors[" + std::to_string(i) +
                     "] hooks must be an object, ignoring hooks");
        }
        if (item.contains("auth_error_scope")) {
            const auto& scope = item["auth_error_scope"];
            if (scope.is_object() && scope.contains("base_url_prefix") &&
                scope["base_url_prefix"].is_string()) {
                connector.auth_error_base_url_prefix =
                    trim_ascii_copy(scope["base_url_prefix"].get<std::string>());
            } else if (scope.is_object() && scope.contains("base_url_prefix")) {
                LOG_WARN("[config] connectors[" + std::to_string(i) +
                         "] auth_error_scope.base_url_prefix must be a string, ignoring");
            } else if (!scope.is_object()) {
                LOG_WARN("[config] connectors[" + std::to_string(i) +
                         "] auth_error_scope must be an object, ignoring");
            }
        }
        if (connector.id.empty() || connector.name.empty()) {
            LOG_WARN("[config] connectors[" + std::to_string(i) +
                     "] has empty id or name, skipping");
            continue;
        }
        if (!seen_ids.insert(connector.id).second) {
            LOG_WARN("[config] duplicate connector id '" + connector.id + "', skipping");
            continue;
        }
        parsed.push_back(std::move(connector));
    }
    out = std::move(parsed);
}

ModelProfile legacy_model_profile_from_config(const AppConfig& cfg) {
    ModelProfile profile;
    profile.name = legacy_model_profile_name(cfg);
    if (cfg.provider == "openai") {
        OpenAiConfig defaults;
        profile.provider = "openai";
        profile.base_url = cfg.openai.base_url.empty()
            ? defaults.base_url
            : cfg.openai.base_url;
        profile.api_key = cfg.openai.api_key;
        profile.model = cfg.openai.model.empty()
            ? defaults.model
            : cfg.openai.model;
        profile.stream_timeout_ms = cfg.openai.stream_timeout_ms;
        profile.request_headers = cfg.openai.request_headers;
        profile.models_dev_provider_id = cfg.openai.models_dev_provider_id;
        return profile;
    }

    if (cfg.provider == "grok") {
        profile.provider = "grok";
        profile.model = "grok-4.5";
        profile.models_dev_provider_id = "xai";
        return profile;
    }

    if (cfg.provider != "copilot") return profile;

    CopilotConfig defaults;
    profile.provider = "copilot";
    profile.model = cfg.copilot.model.empty()
        ? defaults.model
        : cfg.copilot.model;
    return profile;
}

std::string get_acecode_dir() {
    // 数据目录路径解析全部委托给 paths.cpp,RunMode 决定 User vs Service 根目录
    // (Decision 8)。User 模式行为与历史一致 — TUI / standalone daemon 不受影响。
    return resolve_data_dir(get_run_mode());
}

std::string get_run_dir() {
    // desktop 多 workspace 模式下 daemon 启动时会调 set_run_dir_override,
    // 把 run/ 切到 per-workspace 路径(避免共享 ~/.acecode/run/ 互相覆盖锁文件)。
    auto override_path = get_run_dir_override();
    if (!override_path.empty()) return override_path;
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / constants::SUBDIR_RUN);
}

std::string get_logs_dir() {
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / constants::SUBDIR_LOGS);
}

std::vector<std::string> validate_config(const AppConfig& cfg) {
    std::vector<std::string> errors;
    if (cfg.web.port < 1 || cfg.web.port > 65535) {
        errors.push_back("web.port out of range (1-65535): " + std::to_string(cfg.web.port));
    }
    if (cfg.web.bind.empty()) {
        errors.push_back("web.bind is empty; expected an IP address (e.g. 127.0.0.1)");
    }
    if (!is_valid_web_ui_theme(cfg.web_ui.theme)) {
        errors.push_back("web_ui.theme must be one of: system, light, dark");
    }
    if (!is_valid_web_ui_color_theme(cfg.web_ui.color_theme)) {
        errors.push_back("web_ui.color_theme must be blue, orange, eva-01, national-day-2026, or a valid ai- theme ID");
    }
    if (!is_valid_web_ui_font_size(cfg.web_ui.font_size)) {
        errors.push_back("web_ui.font_size must be one of: small, medium, large");
    }
    if (cfg.web.remote_port < 0 || cfg.web.remote_port > 65535) {
        errors.push_back(
            "web.remote_port out of range (0-65535): " +
            std::to_string(cfg.web.remote_port));
    } else if (cfg.web.remote_port != 0 &&
               cfg.web.remote_port == cfg.web.port) {
        errors.push_back(
            "web.remote_port must differ from web.port because the reverse "
            "proxy and daemon cannot share a wildcard port");
    }
    if (cfg.daemon.heartbeat_interval_ms <= 0) {
        errors.push_back("daemon.heartbeat_interval_ms must be > 0");
    }
    if (cfg.daemon.heartbeat_timeout_ms <= cfg.daemon.heartbeat_interval_ms) {
        errors.push_back("daemon.heartbeat_timeout_ms must be > daemon.heartbeat_interval_ms");
    }
    if (cfg.daemon.service_name.empty()) {
        errors.push_back("daemon.service_name is empty");
    }
    if (cfg.memory.max_index_bytes == 0) {
        errors.push_back("memory.max_index_bytes must be > 0");
    }
    if (cfg.ask.max_questions < 1 || cfg.ask.max_questions > 50) {
        errors.push_back("ask.max_questions out of range (1-50): " +
                         std::to_string(cfg.ask.max_questions));
    }
    if (cfg.ask.max_options < 4 || cfg.ask.max_options > 8) {
        errors.push_back("ask.max_options out of range (4-8): " +
                         std::to_string(cfg.ask.max_options));
    }
    if (cfg.openai.stream_timeout_ms <= 0) {
        errors.push_back("openai.stream_timeout_ms must be > 0");
    }
    if (cfg.session_title.max_input_bytes < 1 || cfg.session_title.max_input_bytes > 20000) {
        errors.push_back("session_title.max_input_bytes out of range (1-20000)");
    }
    if (cfg.session_title.timeout_ms < 1000 || cfg.session_title.timeout_ms > 120000) {
        errors.push_back("session_title.timeout_ms out of range (1000-120000)");
    }
    if (!cfg.openai.request_headers.empty()) {
        std::string err;
        if (!validate_request_headers(cfg.openai.request_headers, err)) {
            errors.push_back("openai." + err);
        }
    }
    if (cfg.project_instructions.max_depth < 1) {
        errors.push_back("project_instructions.max_depth must be >= 1");
    }
    if (cfg.project_instructions.max_bytes == 0) {
        errors.push_back("project_instructions.max_bytes must be > 0");
    }
    if (cfg.project_instructions.max_total_bytes < cfg.project_instructions.max_bytes) {
        errors.push_back("project_instructions.max_total_bytes must be >= max_bytes");
    }
    if (cfg.custom_instructions.text_snapshot().size() > kCustomInstructionsMaxBytes) {
        errors.push_back("custom_instructions.text exceeds " +
                         std::to_string(kCustomInstructionsMaxBytes) + " bytes");
    }
    std::set<std::string> connector_ids;
    for (const auto& connector : cfg.connectors) {
        if (trim_ascii_copy(connector.id).empty()) {
            errors.push_back("connectors.id must not be empty");
        } else if (!connector_ids.insert(trim_ascii_copy(connector.id)).second) {
            errors.push_back("connectors.id must be unique: " + connector.id);
        }
        if (connector.name.empty()) {
            errors.push_back("connectors.name must not be empty for id: " +
                             connector.id);
        }
    }
    if (!is_valid_upgrade_base_url(cfg.upgrade.base_url)) {
        errors.push_back("upgrade.base_url must be a non-empty http or https URL");
    }
    if (cfg.upgrade.timeout_ms < 1000 || cfg.upgrade.timeout_ms > 120000) {
        errors.push_back("upgrade.timeout_ms out of range (1000-120000): " +
                         std::to_string(cfg.upgrade.timeout_ms));
    }
    if (cfg.remote_control.port < 1 || cfg.remote_control.port > 65535) {
        errors.push_back("remote_control.port out of range (1-65535): " +
                         std::to_string(cfg.remote_control.port));
    }
    if (!cfg.remote_control.default_channel.empty() &&
        cfg.remote_control.channels.find(cfg.remote_control.default_channel) ==
            cfg.remote_control.channels.end()) {
        errors.push_back("remote_control.default_channel references an undefined channel: " +
                         cfg.remote_control.default_channel);
    }
    for (const auto& [name, channel] : cfg.remote_control.channels) {
        if (name.empty()) {
            errors.push_back("remote_control.channels contains an empty channel name");
            continue;
        }
        bool bad_name = false;
        for (unsigned char ch : name) {
            if (std::isspace(ch) || ch == '/' || ch == '\\') {
                bad_name = true;
                break;
            }
        }
        if (bad_name) {
            errors.push_back("remote_control.channels." + name +
                             " must not contain whitespace or path separators");
        }
        if (channel.manifest_path.empty()) {
            errors.push_back("remote_control.channels." + name +
                             ".manifest_path must not be empty");
        }
        if (channel.timeout_ms < 1000 || channel.timeout_ms > 120000) {
            errors.push_back("remote_control.channels." + name +
                             ".timeout_ms out of range (1000-120000): " +
                             std::to_string(channel.timeout_ms));
        }
        if (!channel.settings.is_object()) {
            errors.push_back("remote_control.channels." + name +
                             ".settings must be a JSON object");
        }
    }
    for (const auto& fn : cfg.project_instructions.filenames) {
        if (fn.empty()) {
            errors.push_back("project_instructions.filenames contains empty entry");
            break;
        }
        if (fn.find('/') != std::string::npos || fn.find('\\') != std::string::npos) {
            errors.push_back("project_instructions.filenames entry must not contain path separator: " + fn);
            break;
        }
    }
    return errors;
}

static void write_default_config(const std::string& config_path) {
    nlohmann::json j;
    j["provider"] = "";
    j["openai"]["base_url"] = "http://localhost:1234/v1";
    j["openai"]["api_key"] = "";
    j["openai"]["model"] = "local-model";
    j["copilot"]["model"] = "gpt-4o";
    j["codex"]["model"] = "gpt-5.5";
    j["saved_models"] = nlohmann::json::array();
    j["default_model_name"] = "";
    j["default_permission_mode"] = "default";
    j["ui"]["locale"] = "auto";

    std::ofstream ofs(path_from_utf8(config_path));
    if (ofs.is_open()) {
        ofs << j.dump(2) << std::endl;
    }
}

static void synthesize_legacy_saved_model_if_needed(AppConfig& cfg,
                                                    bool saved_models_key_present) {
    if (!cfg.saved_models.empty()) return;
    if (saved_models_key_present) {
        if (!cfg.default_model_name.empty()) {
            LOG_WARN("[config] default_model_name ignored because saved_models is empty: " +
                     cfg.default_model_name);
            cfg.default_model_name.clear();
        }
        return;
    }

    ModelProfile legacy = legacy_model_profile_from_config(cfg);
    std::vector<ModelProfile> candidate{legacy};
    std::string err;
    if (validate_saved_models(candidate, legacy.name, err)) {
        cfg.saved_models = std::move(candidate);
        cfg.default_model_name = legacy.name;
        LOG_WARN("[config] saved_models missing; synthesized legacy model profile '" +
                 legacy.name + "' from provider/openai/copilot/codex fields");
        return;
    }

    if (!cfg.default_model_name.empty()) {
        LOG_WARN("[config] default_model_name ignored because saved_models is empty: " +
                 cfg.default_model_name);
        cfg.default_model_name.clear();
    }
    LOG_WARN("[config] saved_models missing and legacy fields cannot be migrated: " + err);
}

static const ModelProfile* find_profile_by_name(const std::vector<ModelProfile>& entries,
                                                const std::string& name) {
    if (name.empty()) return nullptr;
    for (const auto& entry : entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

static const ModelProfile* first_enabled_profile(const std::vector<ModelProfile>& entries) {
    for (const auto& entry : entries) {
        if (is_runtime_model_provider_enabled(entry.provider)) return &entry;
    }
    return nullptr;
}

static void sanitize_disabled_model_providers(AppConfig& cfg) {
    bool provider_was_disabled = false;
    if (!cfg.provider.empty() && !is_runtime_model_provider_enabled(cfg.provider)) {
        LOG_WARN(std::string("[config] provider '") + cfg.provider +
                 "' is disabled; falling back to an enabled saved model");
        provider_was_disabled = true;
    }

    if (cfg.saved_models.empty()) {
        if (provider_was_disabled) cfg.provider.clear();
        return;
    }

    const ModelProfile* default_profile =
        find_profile_by_name(cfg.saved_models, cfg.default_model_name);
    if (default_profile &&
        is_runtime_model_provider_enabled(default_profile->provider)) {
        if (provider_was_disabled) cfg.provider = default_profile->provider;
        return;
    }

    if (default_profile) {
        LOG_WARN(std::string("[config] default model '") + cfg.default_model_name +
                 "' uses disabled provider '" + default_profile->provider + "'");
    }

    if (const ModelProfile* fallback = first_enabled_profile(cfg.saved_models)) {
        if (cfg.default_model_name != fallback->name) {
            LOG_WARN("[config] switching default model to enabled profile '" +
                     fallback->name + "'");
        }
        cfg.default_model_name = fallback->name;
        cfg.provider = fallback->provider;
        return;
    }

    LOG_WARN("[config] no enabled saved model profiles; clearing default model");
    cfg.default_model_name.clear();
    cfg.provider.clear();
}

AppConfig load_config() {
    const std::string config_path =
        path_to_utf8(path_from_utf8(get_acecode_dir()) / "config.json");
    AppConfig cfg = load_config_from_path(config_path, true);
    if (!cfg.sandbox_disable_migration_completed) {
        const auto migration = disable_sandbox_once(config_path);
        if (!migration.ok) {
            throw std::runtime_error(
                "failed to persist one-time sandbox migration: " + migration.error);
        }
        // Reapply runtime-only environment overrides after the locked disk update.
        cfg = load_config_from_path(config_path, true);
    }
    return cfg;
}

static AppConfig load_config_from_path_once(
    const std::string& explicit_path,
    bool apply_environment_overrides,
    std::optional<std::string>* proven_persisted_bytes = nullptr) {
    AppConfig cfg;
    bool saved_models_key_present = false;
    std::optional<std::string> active_bytes;
    if (proven_persisted_bytes) proven_persisted_bytes->reset();

    fs::path native_config_path = path_from_utf8(explicit_path);
    fs::path native_acecode_dir = native_config_path.parent_path();
    const std::string config_path = path_to_utf8(native_config_path);

    // Create directory and default config if missing
    std::error_code home_ec;
    bool home_exists =
        native_acecode_dir.empty() || fs::exists(native_acecode_dir, home_ec);
    if (home_ec) home_exists = false;
    if (!home_exists) {
        fs::create_directories(native_acecode_dir);
        g_acecode_home_created_by_process.store(true);
    }
    if (!fs::exists(native_config_path)) {
        write_default_config(config_path);
    }

    // Read config file
    std::ifstream ifs(native_config_path, std::ios::binary);
    if (ifs.is_open()) {
        try {
            std::ostringstream raw_stream;
            raw_stream << ifs.rdbuf();
            if (ifs.bad()) {
                throw ConfigLoadFailure(
                    "filesystem_read",
                    "failed to read config file: " + config_path);
            }
            active_bytes = raw_stream.str();
            nlohmann::json j = nlohmann::json::parse(*active_bytes);
            ifs.close();
            if (recover_mcp_config(j, config_path)) {
                active_bytes = j.dump(2) + "\n";
            }

            if (j.contains("provider") && j["provider"].is_string()) {
                cfg.provider = j["provider"].get<std::string>();
            }
            if (j.contains("openai") && j["openai"].is_object()) {
                auto& oj = j["openai"];
                if (oj.contains("base_url") && oj["base_url"].is_string())
                    cfg.openai.base_url = oj["base_url"].get<std::string>();
                if (oj.contains("api_key") && oj["api_key"].is_string())
                    cfg.openai.api_key = oj["api_key"].get<std::string>();
                if (oj.contains("model") && oj["model"].is_string())
                    cfg.openai.model = oj["model"].get<std::string>();
                if (oj.contains("stream_timeout_ms") &&
                    oj["stream_timeout_ms"].is_number_integer()) {
                    int v = oj["stream_timeout_ms"].get<int>();
                    if (v <= 0) {
                        fatal_config_value("openai.stream_timeout_ms=" +
                                           std::to_string(v) +
                                           " out of range (>0)");
                    }
                    cfg.openai.stream_timeout_ms = v;
                }
                if (oj.contains("models_dev_provider_id") &&
                    oj["models_dev_provider_id"].is_string()) {
                    cfg.openai.models_dev_provider_id =
                        oj["models_dev_provider_id"].get<std::string>();
                }
                if (oj.contains("request_headers")) {
                    std::string err;
                    auto parsed = parse_request_headers_json(
                        oj["request_headers"],
                        "openai",
                        err);
                    if (!parsed.has_value()) {
                        fatal_config_value(err);
                    }
                    cfg.openai.request_headers = std::move(*parsed);
                }
            }
            if (j.contains("copilot") && j["copilot"].is_object()) {
                auto& cj = j["copilot"];
                if (cj.contains("model") && cj["model"].is_string())
                    cfg.copilot.model = cj["model"].get<std::string>();
            }
            if (j.contains("codex") && j["codex"].is_object()) {
                auto& cj = j["codex"];
                if (cj.contains("model") && cj["model"].is_string())
                    cfg.codex.model = cj["model"].get<std::string>();
            }
            if (j.contains("context_window") && j["context_window"].is_number_integer()) {
                cfg.context_window = j["context_window"].get<int>();
            }
            if (j.contains("max_sessions") && j["max_sessions"].is_number_integer()) {
                cfg.max_sessions = j["max_sessions"].get<int>();
            }
            if (j.contains("task_suggestion_compact_threshold") &&
                j["task_suggestion_compact_threshold"].is_number_integer()) {
                const auto& threshold = j["task_suggestion_compact_threshold"];
                if (threshold >= 0 && threshold <= 1000) {
                    cfg.task_suggestion_compact_threshold = threshold.get<int>();
                }
            }
            if (j.contains("default_permission_mode") &&
                j["default_permission_mode"].is_string()) {
                cfg.default_permission_mode = normalize_permission_mode_name(
                    j["default_permission_mode"].get<std::string>());
            }
            if (j.contains("migrations") && j["migrations"].is_object()) {
                const auto& migrations = j["migrations"];
                cfg.sandbox_disable_migration_completed =
                    migrations.contains("disable_sandbox_once") &&
                    migrations["disable_sandbox_once"] == true;
            }
            // 沙盒段(openspec add-auto-mode-sandbox)。缺省 → enabled、不放行
            // 网络、无额外可写根。非法条目静默跳过,不阻塞启动。
            if (j.contains("sandbox") && j["sandbox"].is_object()) {
                const auto& sj = j["sandbox"];
                if (sj.contains("enabled") && sj["enabled"].is_boolean()) {
                    cfg.sandbox.enabled = sj["enabled"].get<bool>();
                }
                if (sj.contains("network_access") && sj["network_access"].is_boolean()) {
                    cfg.sandbox.network_access = sj["network_access"].get<bool>();
                }
                if (sj.contains("exclude_tmpdir") && sj["exclude_tmpdir"].is_boolean()) {
                    cfg.sandbox.exclude_tmpdir = sj["exclude_tmpdir"].get<bool>();
                }
                if (sj.contains("writable_roots") && sj["writable_roots"].is_array()) {
                    for (const auto& item : sj["writable_roots"]) {
                        if (item.is_string() && !item.get<std::string>().empty() &&
                            path_from_utf8(item.get<std::string>()).is_absolute()) {
                            cfg.sandbox.writable_roots.push_back(item.get<std::string>());
                        }
                    }
                }
                // 权限清单(openspec align-codex-sandboxing):条目原文保留(记号
                // 在策略构造时展开),只要求是非空字符串;非法项静默跳过。
                auto read_entries = [](const nlohmann::json& node, std::vector<std::string>& out) {
                    if (!node.is_array()) return;
                    for (const auto& item : node) {
                        if (item.is_string() && !item.get<std::string>().empty()) {
                            out.push_back(item.get<std::string>());
                        }
                    }
                };
                if (sj.contains("filesystem") && sj["filesystem"].is_object()) {
                    const auto& fj = sj["filesystem"];
                    if (fj.contains("read")) read_entries(fj["read"], cfg.sandbox.filesystem_read);
                    if (fj.contains("write")) read_entries(fj["write"], cfg.sandbox.filesystem_write);
                    if (fj.contains("deny")) read_entries(fj["deny"], cfg.sandbox.filesystem_deny);
                }
                if (sj.contains("deny_defaults") && sj["deny_defaults"].is_boolean()) {
                    cfg.sandbox.deny_defaults = sj["deny_defaults"].get<bool>();
                }
                if (sj.contains("windows_backend") && sj["windows_backend"].is_string()) {
                    const auto backend = sj["windows_backend"].get<std::string>();
                    if (backend == "restricted-token" || backend == "mxc") cfg.sandbox.windows_backend = backend;
                }
            }
            if (j.contains("features") && j["features"].is_object()) {
                const auto& fj = j["features"];
                if (fj.contains("hooks") && fj["hooks"].is_boolean()) {
                    cfg.features.hooks = fj["hooks"].get<bool>();
                }
                if (fj.contains("completed_turn_self_heal") &&
                    fj["completed_turn_self_heal"].is_boolean()) {
                    cfg.features.completed_turn_self_heal =
                        fj["completed_turn_self_heal"].get<bool>();
                }
            }
            if (j.contains("skills") && j["skills"].is_object()) {
                const auto& sj = j["skills"];
                if (sj.contains("disabled") && sj["disabled"].is_array()) {
                    for (const auto& v : sj["disabled"]) {
                        if (v.is_string()) cfg.skills.disabled.push_back(v.get<std::string>());
                    }
                }
                if (sj.contains("external_dirs") && sj["external_dirs"].is_array()) {
                    for (const auto& v : sj["external_dirs"]) {
                        if (v.is_string()) cfg.skills.external_dirs.push_back(v.get<std::string>());
                    }
                }
                if (sj.contains("reuse_opencode") && sj["reuse_opencode"].is_boolean()) {
                    cfg.skills.reuse_opencode = sj["reuse_opencode"].get<bool>();
                }
                if (sj.contains("idle_days") && sj["idle_days"].is_number_integer()) {
                    cfg.skills.idle_days = sj["idle_days"].get<int>();
                }
            }
            if (j.contains("memory") && j["memory"].is_object()) {
                const auto& mj = j["memory"];
                if (mj.contains("enabled") && mj["enabled"].is_boolean())
                    cfg.memory.enabled = mj["enabled"].get<bool>();
                if (mj.contains("max_index_bytes") && mj["max_index_bytes"].is_number_integer()) {
                    long long v = mj["max_index_bytes"].get<long long>();
                    if (v > 0) cfg.memory.max_index_bytes = static_cast<std::size_t>(v);
                }
            }
            if (j.contains("project_instructions") && j["project_instructions"].is_object()) {
                const auto& pj = j["project_instructions"];
                if (pj.contains("enabled") && pj["enabled"].is_boolean())
                    cfg.project_instructions.enabled = pj["enabled"].get<bool>();
                if (pj.contains("max_depth") && pj["max_depth"].is_number_integer()) {
                    int v = pj["max_depth"].get<int>();
                    if (v > 0) cfg.project_instructions.max_depth = v;
                }
                if (pj.contains("max_bytes") && pj["max_bytes"].is_number_integer()) {
                    long long v = pj["max_bytes"].get<long long>();
                    if (v > 0) cfg.project_instructions.max_bytes = static_cast<std::size_t>(v);
                }
                if (pj.contains("max_total_bytes") && pj["max_total_bytes"].is_number_integer()) {
                    long long v = pj["max_total_bytes"].get<long long>();
                    if (v > 0) cfg.project_instructions.max_total_bytes = static_cast<std::size_t>(v);
                }
                if (pj.contains("filenames") && pj["filenames"].is_array()) {
                    std::vector<std::string> fns;
                    for (const auto& v : pj["filenames"]) {
                        if (v.is_string()) {
                            std::string s = v.get<std::string>();
                            if (!s.empty()) fns.push_back(std::move(s));
                        }
                    }
                    // Empty array -> keep the struct's default list so
                    // AGENT.md / CLAUDE.md still work out of the box.
                    if (!fns.empty()) cfg.project_instructions.filenames = std::move(fns);
                }
                if (pj.contains("read_claude_md") && pj["read_claude_md"].is_boolean())
                    cfg.project_instructions.read_claude_md = pj["read_claude_md"].get<bool>();
            }
            if (j.contains("custom_instructions") && j["custom_instructions"].is_object()) {
                const auto& cj = j["custom_instructions"];
                if (cj.contains("text") && cj["text"].is_string()) {
                    cfg.custom_instructions.set_text(cj["text"].get<std::string>());
                }
            }
            if (j.contains("connectors")) {
                load_connectors_lenient(j["connectors"], cfg.connectors);
            }
            if (j.contains("daemon") && j["daemon"].is_object()) {
                const auto& dj = j["daemon"];
                if (dj.contains("auto_start_on_double_click") && dj["auto_start_on_double_click"].is_boolean())
                    cfg.daemon.auto_start_on_double_click = dj["auto_start_on_double_click"].get<bool>();
                if (dj.contains("service_name") && dj["service_name"].is_string())
                    cfg.daemon.service_name = dj["service_name"].get<std::string>();
                if (dj.contains("heartbeat_interval_ms") && dj["heartbeat_interval_ms"].is_number_integer())
                    cfg.daemon.heartbeat_interval_ms = dj["heartbeat_interval_ms"].get<int>();
                if (dj.contains("heartbeat_timeout_ms") && dj["heartbeat_timeout_ms"].is_number_integer())
                    cfg.daemon.heartbeat_timeout_ms = dj["heartbeat_timeout_ms"].get<int>();
            }
            if (j.contains("web") && j["web"].is_object()) {
                const auto& wj = j["web"];
                const bool remote_enabled_explicit =
                    wj.contains("remote_enabled") &&
                    wj["remote_enabled"].is_boolean();
                if (wj.contains("enabled") && wj["enabled"].is_boolean())
                    cfg.web.enabled = wj["enabled"].get<bool>();
                if (wj.contains("bind") && wj["bind"].is_string())
                    cfg.web.bind = wj["bind"].get<std::string>();
                if (wj.contains("port") && wj["port"].is_number_integer())
                    cfg.web.port = wj["port"].get<int>();
                if (remote_enabled_explicit)
                    cfg.web.remote_enabled = wj["remote_enabled"].get<bool>();
                if (wj.contains("remote_port") && wj["remote_port"].is_number_integer())
                    cfg.web.remote_port = wj["remote_port"].get<int>();
                // Legacy remote-Web mode used a non-loopback daemon bind as
                // the persisted flag. Migrate intent only when the new flag
                // is absent, then keep the daemon canonical and local.
                if (!cfg.web.bind.empty()) {
                    if (!web_bind_is_loopback(cfg.web.bind) &&
                        !remote_enabled_explicit) {
                        cfg.web.remote_enabled = true;
                    }
                    // All accepted legacy loopback aliases and external binds
                    // converge on the one daemon runtime address.
                    cfg.web.bind = "127.0.0.1";
                }
                // static_dir is intentionally optional. null/missing -> embedded assets;
                // string -> filesystem path. Empty string is treated the same as null.
                if (wj.contains("static_dir") && wj["static_dir"].is_string())
                    cfg.web.static_dir = wj["static_dir"].get<std::string>();
            }
            if (j.contains("web_ui")) {
                if (!j["web_ui"].is_object()) {
                    LOG_WARN("[config] 'web_ui' must be an object, ignoring");
                } else {
                    const auto& uij = j["web_ui"];
                    if (uij.contains("theme")) {
                        if (uij["theme"].is_string() &&
                            is_valid_web_ui_theme(uij["theme"].get<std::string>())) {
                            cfg.web_ui.theme = uij["theme"].get<std::string>();
                        } else {
                            LOG_WARN("[config] invalid 'web_ui.theme', using 'system'");
                        }
                    }
                    if (uij.contains("color_theme")) {
                        if (uij["color_theme"].is_string() &&
                            is_valid_web_ui_color_theme(
                                uij["color_theme"].get<std::string>())) {
                            cfg.web_ui.color_theme =
                                uij["color_theme"].get<std::string>();
                        } else {
                            LOG_WARN("[config] invalid 'web_ui.color_theme', using 'blue'");
                        }
                    }
                    if (uij.contains("font_size")) {
                        if (uij["font_size"].is_string() &&
                            is_valid_web_ui_font_size(
                                uij["font_size"].get<std::string>())) {
                            cfg.web_ui.font_size =
                                uij["font_size"].get<std::string>();
                        } else {
                            LOG_WARN("[config] invalid 'web_ui.font_size', using 'medium'");
                        }
                    }
                    if (uij.contains("message_auto_collapse")) {
                        if (uij["message_auto_collapse"].is_boolean()) {
                            cfg.web_ui.message_auto_collapse =
                                uij["message_auto_collapse"].get<bool>();
                        } else {
                            LOG_WARN("[config] invalid 'web_ui.message_auto_collapse', using true");
                        }
                    }
                    if (uij.contains("sidebar_session_time")) {
                        if (uij["sidebar_session_time"].is_boolean()) {
                            cfg.web_ui.sidebar_session_time =
                                uij["sidebar_session_time"].get<bool>();
                        } else {
                            LOG_WARN("[config] invalid 'web_ui.sidebar_session_time', using true");
                        }
                    }
                }
            }
            if (j.contains("models_dev") && j["models_dev"].is_object()) {
                const auto& mj = j["models_dev"];
                if (mj.contains("allow_network") && mj["allow_network"].is_boolean())
                    cfg.models_dev.allow_network = mj["allow_network"].get<bool>();
                if (mj.contains("refresh_on_command_only") && mj["refresh_on_command_only"].is_boolean())
                    cfg.models_dev.refresh_on_command_only = mj["refresh_on_command_only"].get<bool>();
                if (mj.contains("user_override_path") && mj["user_override_path"].is_string()) {
                    std::string p = mj["user_override_path"].get<std::string>();
                    if (!p.empty()) cfg.models_dev.user_override_path = p;
                }
            }
            if (j.contains("input_history") && j["input_history"].is_object()) {
                const auto& ihj = j["input_history"];
                if (ihj.contains("enabled") && ihj["enabled"].is_boolean())
                    cfg.input_history.enabled = ihj["enabled"].get<bool>();
                if (ihj.contains("max_entries") && ihj["max_entries"].is_number_integer()) {
                    int v = ihj["max_entries"].get<int>();
                    if (v > 0) cfg.input_history.max_entries = v;
                }
            }
            if (j.contains("upgrade")) {
                if (!j["upgrade"].is_object()) {
                    LOG_WARN("[config] 'upgrade' must be an object, ignoring");
                } else {
                    const auto& uj = j["upgrade"];
                    if (uj.contains("base_url") && uj["base_url"].is_string()) {
                        cfg.upgrade.base_url =
                            normalize_upgrade_base_url(uj["base_url"].get<std::string>());
                    }
                    if (uj.contains("timeout_ms") && uj["timeout_ms"].is_number_integer()) {
                        cfg.upgrade.timeout_ms = uj["timeout_ms"].get<int>();
                    }
                }
            }
            if (j.contains("network") && j["network"].is_object()) {
                const auto& nj = j["network"];
                if (nj.contains("proxy_mode") && nj["proxy_mode"].is_string()) {
                    std::string m = nj["proxy_mode"].get<std::string>();
                    if (m != "auto" && m != "off" && m != "manual") {
                        fatal_config_value(
                            "network.proxy_mode is invalid; expected one of: "
                            "auto, off, manual");
                    }
                    cfg.network.proxy_mode = std::move(m);
                }
                if (nj.contains("proxy_url") && nj["proxy_url"].is_string())
                    cfg.network.proxy_url = nj["proxy_url"].get<std::string>();
                if (nj.contains("proxy_no_proxy") && nj["proxy_no_proxy"].is_string())
                    cfg.network.proxy_no_proxy = nj["proxy_no_proxy"].get<std::string>();
                if (nj.contains("proxy_probe_enabled") &&
                    nj["proxy_probe_enabled"].is_boolean())
                    cfg.network.proxy_probe_enabled =
                        nj["proxy_probe_enabled"].get<bool>();
                if (nj.contains("proxy_probe_timeout_ms") &&
                    nj["proxy_probe_timeout_ms"].is_number_integer()) {
                    int raw = nj["proxy_probe_timeout_ms"].get<int>();
                    int clamped = raw;
                    if (clamped < 200)   clamped = 200;
                    if (clamped > 10000) clamped = 10000;
                    if (clamped != raw) {
                        LOG_WARN("[config] network.proxy_probe_timeout_ms=" +
                                 std::to_string(raw) + " out of [200, 10000], clamped to " +
                                 std::to_string(clamped));
                    }
                    cfg.network.proxy_probe_timeout_ms = clamped;
                }

                if (cfg.network.proxy_mode == "manual" && cfg.network.proxy_url.empty()) {
                    fatal_config_value(
                        "network.proxy_mode='manual' requires non-empty "
                        "network.proxy_url");
                }
            }
            // 联网搜索段。缺省 → RSS + DuckDuckGo 并行搜索。
            // 参见 openspec/changes/integrate-rss-web-search/。
            if (j.contains("web_search") && j["web_search"].is_object()) {
                const auto& wsj = j["web_search"];
                if (wsj.contains("enabled") && wsj["enabled"].is_boolean())
                    cfg.web_search.enabled = wsj["enabled"].get<bool>();
                if (wsj.contains("backend") && wsj["backend"].is_string()) {
                    std::string b = wsj["backend"].get<std::string>();
                    if (b != "parallel" && b != "rss" && b != "auto" &&
                        b != "duckduckgo" && b != "bing_cn" &&
                        b != "bochaai" && b != "tavily") {
                        fatal_config_value(
                            "web_search.backend is invalid; expected one of: "
                            "parallel, rss, auto, duckduckgo, bing_cn, "
                            "bochaai, tavily");
                    }
                    cfg.web_search.backend = std::move(b);
                }
                if (wsj.contains("api_key") && wsj["api_key"].is_string())
                    cfg.web_search.api_key = wsj["api_key"].get<std::string>();
                if (wsj.contains("rss_base_url") && wsj["rss_base_url"].is_string()) {
                    const std::string url = wsj["rss_base_url"].get<std::string>();
                    if (!utils::is_valid_http_base_url(url)) {
                        fatal_config_value(
                            "web_search.rss_base_url must be an HTTPS base URL "
                            "(HTTP is allowed only for loopback)");
                    }
                    cfg.web_search.rss_base_url = url;
                }
                if (wsj.contains("max_results") && wsj["max_results"].is_number_integer()) {
                    int v = wsj["max_results"].get<int>();
                    if (v < 1 || v > 10) {
                        fatal_config_value(
                            "web_search.max_results out of range (1..10)");
                    }
                    cfg.web_search.max_results = v;
                }
                if (wsj.contains("timeout_ms") && wsj["timeout_ms"].is_number_integer()) {
                    int v = wsj["timeout_ms"].get<int>();
                    if (v < 1000 || v > 30000) {
                        fatal_config_value(
                            "web_search.timeout_ms out of range (1000..30000)");
                    }
                    cfg.web_search.timeout_ms = v;
                }
            }

            // 图像生成段(openspec add-image-generation-tool)。缺省 → 未配置,
            // 工具不注册。三档模型名允许部分覆盖,未给的沿用默认。
            if (j.contains("image_generation") && j["image_generation"].is_object()) {
                const auto& igj = j["image_generation"];
                if (igj.contains("enabled") && igj["enabled"].is_boolean())
                    cfg.image_generation.enabled = igj["enabled"].get<bool>();
                if (igj.contains("source") && igj["source"].is_string()) {
                    std::string src = igj["source"].get<std::string>();
                    if (src != "saved_model" && src != "inline") {
                        fatal_config_value(
                            "image_generation.source is invalid; expected "
                            "\"saved_model\" or \"inline\"");
                    }
                    cfg.image_generation.source = std::move(src);
                }
                if (igj.contains("saved_model_name") && igj["saved_model_name"].is_string())
                    cfg.image_generation.saved_model_name =
                        igj["saved_model_name"].get<std::string>();
                if (igj.contains("base_url") && igj["base_url"].is_string()) {
                    const std::string url = igj["base_url"].get<std::string>();
                    if (!url.empty() && !utils::is_valid_http_base_url(url)) {
                        fatal_config_value(
                            "image_generation.base_url must be an HTTPS base URL "
                            "(HTTP is allowed only for loopback)");
                    }
                    cfg.image_generation.base_url = url;
                }
                if (igj.contains("api_key") && igj["api_key"].is_string())
                    cfg.image_generation.api_key = igj["api_key"].get<std::string>();
                if (igj.contains("models") && igj["models"].is_object()) {
                    const auto& mj = igj["models"];
                    if (mj.contains("standard") && mj["standard"].is_string())
                        cfg.image_generation.model_standard = mj["standard"].get<std::string>();
                    if (mj.contains("high") && mj["high"].is_string())
                        cfg.image_generation.model_high = mj["high"].get<std::string>();
                    if (mj.contains("ultra") && mj["ultra"].is_string())
                        cfg.image_generation.model_ultra = mj["ultra"].get<std::string>();
                }
                if (igj.contains("default_quality") && igj["default_quality"].is_string()) {
                    std::string q = igj["default_quality"].get<std::string>();
                    if (q != "standard" && q != "high" && q != "ultra") {
                        fatal_config_value(
                            "image_generation.default_quality is invalid; expected "
                            "one of: standard, high, ultra");
                    }
                    cfg.image_generation.default_quality = std::move(q);
                }
                if (igj.contains("timeout_ms") && igj["timeout_ms"].is_number_integer()) {
                    // 越界不致命:这里只是一个超时时长,clamp 比把启动搞挂好。
                    // 下限 30s 是因为实测单张就要 20~60s,更小的值等于自杀。
                    cfg.image_generation.timeout_ms = std::clamp(
                        igj["timeout_ms"].get<int>(), 30000, 600000);
                }
            }

            // LSP 段(openspec add-lsp-service)。缺省 → enabled=true、无覆盖。
            // 自定义 server(名字不在内置集合)的 command 校验推迟到注册表合并层
            // (config 层不感知内置名单,避免双份清单漂移)。
            if (j.contains("lsp") && j["lsp"].is_object()) {
                const auto& lspj = j["lsp"];
                if (lspj.contains("enabled") && lspj["enabled"].is_boolean())
                    cfg.lsp.enabled = lspj["enabled"].get<bool>();
                if (lspj.contains("servers") && lspj["servers"].is_object()) {
                    for (const auto& [name, sj] : lspj["servers"].items()) {
                        if (!sj.is_object()) {
                            fatal_config_value(
                                "lsp.servers entry must be an object");
                        }
                        LspServerOverride entry;
                        if (sj.contains("disabled") && sj["disabled"].is_boolean())
                            entry.disabled = sj["disabled"].get<bool>();
                        if (sj.contains("command")) {
                            if (!sj["command"].is_array()) {
                                fatal_config_value(
                                    "lsp.servers command must be a string array");
                            }
                            for (const auto& item : sj["command"]) {
                                if (!item.is_string()) {
                                    fatal_config_value(
                                        "lsp.servers command items must be strings");
                                }
                                entry.command.push_back(item.get<std::string>());
                            }
                        }
                        if (sj.contains("extensions") && sj["extensions"].is_array()) {
                            for (const auto& item : sj["extensions"]) {
                                if (item.is_string())
                                    entry.extensions.push_back(item.get<std::string>());
                            }
                        }
                        if (sj.contains("env") && sj["env"].is_object()) {
                            for (const auto& [k, v] : sj["env"].items()) {
                                if (v.is_string()) entry.env[k] = v.get<std::string>();
                            }
                        }
                        if (sj.contains("initialization") && sj["initialization"].is_object())
                            entry.initialization = sj["initialization"];
                        cfg.lsp.servers[name] = std::move(entry);
                    }
                }
            }

            // Worktree 段。缺省 → 默认值(不 symlink、完整 checkout)。
            // 非法条目(非字符串)静默跳过,不阻塞启动。
            if (j.contains("worktree") && j["worktree"].is_object()) {
                const auto& wtj = j["worktree"];
                if (wtj.contains("symlink_directories") &&
                    wtj["symlink_directories"].is_array()) {
                    for (const auto& item : wtj["symlink_directories"]) {
                        if (item.is_string()) {
                            cfg.worktree.symlink_directories.push_back(
                                item.get<std::string>());
                        }
                    }
                }
                if (wtj.contains("sparse_paths") && wtj["sparse_paths"].is_array()) {
                    for (const auto& item : wtj["sparse_paths"]) {
                        if (item.is_string()) {
                            cfg.worktree.sparse_paths.push_back(item.get<std::string>());
                        }
                    }
                }
            }

            // git 感知段(openspec add-git-context)。缺省 → enabled=true、
            // timeout 3000ms。timeout 越界不 fatal,静默 clamp —— 该值只影响
            // best-effort 的采集行为,不值得阻塞启动。
            if (j.contains("git_context") && j["git_context"].is_object()) {
                const auto& gcj = j["git_context"];
                if (gcj.contains("enabled") && gcj["enabled"].is_boolean())
                    cfg.git_context.enabled = gcj["enabled"].get<bool>();
                if (gcj.contains("timeout_ms") &&
                    gcj["timeout_ms"].is_number_integer()) {
                    int v = gcj["timeout_ms"].get<int>();
                    if (v < 500) v = 500;
                    if (v > 30000) v = 30000;
                    cfg.git_context.timeout_ms = v;
                }
            }

            if (j.contains("remote_control") && j["remote_control"].is_object()) {
                const auto& rcj = j["remote_control"];
                if (rcj.contains("port") && rcj["port"].is_number_integer()) {
                    int v = rcj["port"].get<int>();
                    if (v < 1 || v > 65535) {
                        fatal_config_value(
                            "remote_control.port out of range (1..65535)");
                    }
                    cfg.remote_control.port = v;
                }
                if (rcj.contains("token") && rcj["token"].is_string())
                    cfg.remote_control.token = rcj["token"].get<std::string>();
                if (rcj.contains("outbound_url") && rcj["outbound_url"].is_string())
                    cfg.remote_control.outbound_url = rcj["outbound_url"].get<std::string>();
                if (rcj.contains("default_channel") && rcj["default_channel"].is_string())
                    cfg.remote_control.default_channel =
                        rcj["default_channel"].get<std::string>();
                if (rcj.contains("bound_session_id") && rcj["bound_session_id"].is_string())
                    cfg.remote_control.bound_session_id =
                        rcj["bound_session_id"].get<std::string>();
                if (rcj.contains("channels") && rcj["channels"].is_object()) {
                    for (const auto& item : rcj["channels"].items()) {
                        if (!item.value().is_object()) continue;
                        RemoteControlConfig::ChannelPluginConfig channel;
                        const auto& cj = item.value();
                        if (cj.contains("manifest_path") && cj["manifest_path"].is_string())
                            channel.manifest_path = cj["manifest_path"].get<std::string>();
                        if (cj.contains("timeout_ms") && cj["timeout_ms"].is_number_integer())
                            channel.timeout_ms = cj["timeout_ms"].get<int>();
                        if (cj.contains("settings"))
                            channel.settings = cj["settings"];
                        cfg.remote_control.channels[item.key()] = std::move(channel);
                    }
                }
            }

            // AskUserQuestion 跨端题目数量配置。不存在时保持默认上限 10。
            // 非整数忽略,整数统一钳制到 [1,50]。
            if (j.contains("ask")) {
                if (!j["ask"].is_object()) {
                    LOG_WARN("[config] 'ask' must be an object, ignoring");
                } else {
                    const auto& aj = j["ask"];
                    if (aj.contains("max_questions")) {
                        const auto& value = aj["max_questions"];
                        if (!value.is_number_integer()) {
                            LOG_WARN("[config] ask.max_questions must be an integer; ignoring");
                        } else {
                            const int normalized = clamp_config_integer(value, 1, 50);
                            if (value != normalized) {
                                LOG_WARN("[config] ask.max_questions=" +
                                         value.dump() +
                                         " is outside [1, 50]; clamping to " +
                                         std::to_string(normalized));
                            }
                            cfg.ask.max_questions = normalized;
                        }
                    }
                    // AskUserQuestion 选项数量上限。不存在时保持默认上限 6。
                    // 非整数忽略,整数统一钳制到 [4,8]。
                    if (aj.contains("max_options")) {
                        const auto& value = aj["max_options"];
                        if (!value.is_number_integer()) {
                            LOG_WARN("[config] ask.max_options must be an integer; ignoring");
                        } else {
                            const int normalized = clamp_config_integer(value, 4, 8);
                            if (value != normalized) {
                                LOG_WARN("[config] ask.max_options=" +
                                         value.dump() +
                                         " is outside [4, 8]; clamping to " +
                                         std::to_string(normalized));
                            }
                            cfg.ask.max_options = normalized;
                        }
                    }
                }
            }

            // TUI 渲染策略段。不存在时保持 TuiConfig 默认值(alt_screen_mode="auto")。
            // 非对象类型 + 非法字符串值都规范化到 "auto",启动不阻断。
            if (j.contains("tui")) {
                if (!j["tui"].is_object()) {
                    LOG_WARN("[config] 'tui' must be an object, ignoring");
                } else {
                    const auto& tj = j["tui"];
                    if (tj.contains("alt_screen_mode") && tj["alt_screen_mode"].is_string()) {
                        std::string m = tj["alt_screen_mode"].get<std::string>();
                        if (m == "auto" || m == "always" || m == "never") {
                            cfg.tui.alt_screen_mode = std::move(m);
                        } else {
                            LOG_WARN("[config] invalid tui.alt_screen_mode value '" + m +
                                     "', falling back to 'auto'");
                            cfg.tui.alt_screen_mode = "auto";
                        }
                    }
                    if (tj.contains("sync_output_mode") && tj["sync_output_mode"].is_string()) {
                        std::string m = tj["sync_output_mode"].get<std::string>();
                        if (m == "auto" || m == "always" || m == "never") {
                            cfg.tui.sync_output_mode = std::move(m);
                        } else {
                            LOG_WARN("[config] invalid tui.sync_output_mode value '" + m +
                                     "', falling back to 'auto'");
                            cfg.tui.sync_output_mode = "auto";
                        }
                    }
                    if (tj.contains("page_keys_single_line") &&
                        tj["page_keys_single_line"].is_boolean()) {
                        cfg.tui.page_keys_single_line =
                            tj["page_keys_single_line"].get<bool>();
                    }
                    if (tj.contains("theme") && tj["theme"].is_string()) {
                        std::string t = tj["theme"].get<std::string>();
                        if (t == "auto" || t == "dark" || t == "light") {
                            cfg.tui.theme = std::move(t);
                        } else {
                            LOG_WARN("[config] invalid tui.theme value '" + t +
                                     "', falling back to 'auto'");
                            cfg.tui.theme = "auto";
                        }
                    }
                    auto read_tui_integer = [&](const char* key, int minimum,
                                                int maximum, int& target) {
                        if (!tj.contains(key)) return;
                        const auto& value = tj[key];
                        if (!value.is_number_integer()) {
                            LOG_WARN(std::string("[config] tui.") + key +
                                     " must be an integer; ignoring");
                            return;
                        }
                        const int normalized = clamp_config_integer(value, minimum, maximum);
                        if (value != normalized) {
                            LOG_WARN(std::string("[config] tui.") + key + "=" +
                                     value.dump() + " is outside [" +
                                     std::to_string(minimum) + ", " +
                                     std::to_string(maximum) + "]; clamping to " +
                                     std::to_string(normalized));
                        }
                        target = normalized;
                    };
                    read_tui_integer("question_min_visible_rows", 2, 12,
                                     cfg.tui.question_min_visible_rows);
                    read_tui_integer("question_selection_feedback_ms", 0, 1000,
                                     cfg.tui.question_selection_feedback_ms);
                }
            }

            // Desktop/native shell 配置。notifications 同时供 Windows TUI 使用。
            // 字段缺失 / 类型错都走默认值(全 true),启动不阻断。
            if (j.contains("desktop")) {
                if (!j["desktop"].is_object()) {
                    LOG_WARN("[config] 'desktop' must be an object, ignoring");
                } else {
                    const auto& dj = j["desktop"];
                    if (dj.contains("allow_multiple_instances") &&
                        dj["allow_multiple_instances"].is_boolean()) {
                        cfg.desktop.allow_multiple_instances =
                            dj["allow_multiple_instances"].get<bool>();
                    }
                    std::optional<bool> legacy_close_to_tray;
                    if (dj.contains("close_to_tray") && dj["close_to_tray"].is_boolean()) {
                        cfg.desktop.close_to_tray = dj["close_to_tray"].get<bool>();
                        legacy_close_to_tray = cfg.desktop.close_to_tray;
                    }
                    std::optional<std::string> configured_close_behavior;
                    if (dj.contains("close_behavior") &&
                        dj["close_behavior"].is_string()) {
                        configured_close_behavior =
                            dj["close_behavior"].get<std::string>();
                        if (!parse_desktop_close_behavior(*configured_close_behavior)) {
                            LOG_WARN("[config] desktop.close_behavior must be one of "
                                     "ask, minimize_to_tray, exit; ignoring");
                        }
                    }
                    cfg.desktop.close_behavior = resolve_desktop_close_behavior(
                        configured_close_behavior
                            ? std::optional<std::string_view>(*configured_close_behavior)
                            : std::nullopt,
                        legacy_close_to_tray);
                    if (dj.contains("continue_background_process") &&
                        dj["continue_background_process"].is_boolean()) {
                        cfg.desktop.continue_background_process =
                            dj["continue_background_process"].get<bool>();
                    }
                    if (dj.contains("notifications")) {
                        if (!dj["notifications"].is_object()) {
                            LOG_WARN("[config] 'desktop.notifications' must be an object, "
                                     "using defaults");
                        } else {
                            const auto& nj = dj["notifications"];
                            if (nj.contains("enabled") && nj["enabled"].is_boolean()) {
                                cfg.desktop.notifications.enabled = nj["enabled"].get<bool>();
                            }
                            if (nj.contains("on_permission") && nj["on_permission"].is_boolean()) {
                                cfg.desktop.notifications.on_permission =
                                    nj["on_permission"].get<bool>();
                            }
                            if (nj.contains("on_question") && nj["on_question"].is_boolean()) {
                                cfg.desktop.notifications.on_question = nj["on_question"].get<bool>();
                            }
                            if (nj.contains("on_completion") && nj["on_completion"].is_boolean()) {
                                cfg.desktop.notifications.on_completion = nj["on_completion"].get<bool>();
                            }
                            if (nj.contains("suppress_when_focused") &&
                                nj["suppress_when_focused"].is_boolean()) {
                                cfg.desktop.notifications.suppress_when_focused =
                                    nj["suppress_when_focused"].get<bool>();
                            }
                            // desktop.notifications.backend is obsolete: Windows
                            // always uses the self-drawn toast. Ignore leftovers
                            // from older configs without warning noise.
                        }
                    }
                }
            }

            // Fixed Desktop/WebUI locale. Missing remains zh-CN for existing
            // installations; fresh configs contain ui.locale="auto".
            if (j.contains("ui")) {
                if (!j["ui"].is_object()) {
                    LOG_WARN("[config] 'ui' must be an object, ignoring");
                } else {
                    const auto& uij = j["ui"];
                    if (uij.contains("locale")) {
                        if (uij["locale"].is_string()) {
                            const std::string locale = uij["locale"].get<std::string>();
                            if (is_valid_ui_locale(locale)) {
                                cfg.ui.locale = locale;
                            } else {
                                LOG_WARN("[config] invalid ui.locale value '" + locale +
                                         "', falling back to 'zh-CN'");
                            }
                        } else {
                            LOG_WARN("[config] 'ui.locale' must be a string, using 'zh-CN'");
                        }
                    }
                }
            }

            // Web 控制台(add-console-dock):shell 覆盖 + + 旁下拉选择器
            // (default_shell / git_bash_path,见 控制台 Shell 选择器 plan)。
            if (j.contains("console")) {
                if (!j["console"].is_object()) {
                    LOG_WARN("[config] 'console' must be an object, ignoring");
                } else {
                    const auto& cj = j["console"];
                    if (cj.contains("shell") && cj["shell"].is_string()) {
                        cfg.console.shell = cj["shell"].get<std::string>();
                    }
                    if (cj.contains("default_shell") && cj["default_shell"].is_string()) {
                        cfg.console.default_shell = cj["default_shell"].get<std::string>();
                    }
                    if (cj.contains("shell_paths")) {
                        if (!cj["shell_paths"].is_object()) {
                            LOG_WARN("[config] 'console.shell_paths' must be an object, ignoring");
                        } else {
                            for (auto it = cj["shell_paths"].begin();
                                 it != cj["shell_paths"].end(); ++it) {
                                if (!it.value().is_string()) {
                                    LOG_WARN("[config] 'console.shell_paths." + it.key() +
                                             "' must be a string, ignoring");
                                    continue;
                                }
                                const std::string value = it.value().get<std::string>();
                                if (it.key().empty() || value.empty()) continue;
                                cfg.console.shell_paths[it.key()] = value;
                            }
                        }
                    }
                    // legacy:console.git_bash_path 并入 shell_paths["git-bash"];
                    // 已有显式 git-bash 项时以新字段为准。
                    if (cj.contains("git_bash_path") && cj["git_bash_path"].is_string()) {
                        const std::string legacy = cj["git_bash_path"].get<std::string>();
                        if (!legacy.empty() &&
                            cfg.console.shell_paths.find("git-bash") ==
                                cfg.console.shell_paths.end()) {
                            cfg.console.shell_paths["git-bash"] = legacy;
                        }
                    }
                }
            }

            if (j.contains("toolchains")) {
                if (!j["toolchains"].is_object()) {
                    LOG_WARN("[config] 'toolchains' must be an object, ignoring");
                } else {
                    const auto& tj = j["toolchains"];
                    auto read_dir = [&](const char* key, std::string& out) {
                        if (!tj.contains(key)) return;
                        if (!tj[key].is_string()) {
                            LOG_WARN(std::string("[config] 'toolchains.") + key +
                                     "' must be a string, ignoring");
                            return;
                        }
                        out = tj[key].get<std::string>();
                    };
                    read_dir("python", cfg.toolchains.python);
                    read_dir("node", cfg.toolchains.node);
                    read_dir("csharp", cfg.toolchains.csharp);
                }
            }

            if (j.contains("computer_use")) {
                const auto& computer = j["computer_use"];
                if (!computer.is_object() ||
                    (computer.contains("enabled") && !computer["enabled"].is_boolean())) {
                    throw std::runtime_error("computer_use.enabled must be a boolean");
                }
                cfg.computer_use.enabled = computer.value("enabled", false);
                if (computer.contains("pointer_style")) {
                    if (!computer["pointer_style"].is_string() ||
                        !computer_use::pointer_appearance::valid_style(computer["pointer_style"].get<std::string>()))
                        throw std::runtime_error("computer_use.pointer_style must be ace or plain");
                    cfg.computer_use.pointer_style = computer["pointer_style"].get<std::string>();
                }
                if (computer.contains("pointer_color")) {
                    const auto color = computer["pointer_color"].is_string()
                        ? computer_use::pointer_appearance::normalize_color(computer["pointer_color"].get<std::string>())
                        : std::nullopt;
                    if (!color) throw std::runtime_error("computer_use.pointer_color must be #RRGGBB");
                    cfg.computer_use.pointer_color = *color;
                }
            }

            if (j.contains("summary_generation")) {
                const auto& summary = j["summary_generation"];
                if (!summary.is_object() ||
                    (summary.contains("enabled") && !summary["enabled"].is_boolean()) ||
                    (summary.contains("model_name") && !summary["model_name"].is_string())) {
                    throw std::runtime_error("summary_generation must contain a boolean enabled and a string model_name");
                }
                cfg.summary_generation.enabled = summary.value("enabled", false);
                cfg.summary_generation.model_name = summary.value("model_name", std::string{});
            }

            if (j.contains("session_title")) {
                if (!j["session_title"].is_object()) {
                    LOG_WARN("[config] 'session_title' must be an object, ignoring");
                } else {
                    const auto& stj = j["session_title"];
                    if (stj.contains("enabled") && stj["enabled"].is_boolean()) {
                        cfg.session_title.enabled = stj["enabled"].get<bool>();
                    }
                    if (stj.contains("model_name") && stj["model_name"].is_string()) {
                        cfg.session_title.model_name = stj["model_name"].get<std::string>();
                    }
                    if (stj.contains("max_input_bytes") &&
                        stj["max_input_bytes"].is_number_integer()) {
                        cfg.session_title.max_input_bytes = stj["max_input_bytes"].get<int>();
                    }
                    if (stj.contains("timeout_ms") && stj["timeout_ms"].is_number_integer()) {
                        cfg.session_title.timeout_ms = stj["timeout_ms"].get<int>();
                    }
                }
            }

            if (j.contains("agent_loop") && j["agent_loop"].is_object()) {
                const auto& alj = j["agent_loop"];
                // max_iterations = 0 disables the cap. Positive values are
                // clamped to [1, 10000].
                if (alj.contains("max_iterations") && alj["max_iterations"].is_number_integer()) {
                    int v = alj["max_iterations"].get<int>();
                    if (v < 0) {
                        LOG_WARN("[config] agent_loop.max_iterations=" + std::to_string(v) +
                                 " is out of range (min 0); clamping to 0");
                        v = 0;
                    } else if (v > 10000) {
                        LOG_WARN("[config] agent_loop.max_iterations=" + std::to_string(v) +
                                 " is out of range (max 10000); clamping to 10000");
                        v = 10000;
                    }
                    cfg.agent_loop.max_iterations = v;
                }
                // AskUserQuestion 应答策略(add-ask-question-policy)。显式含键
                // = 置 explicit 标记(即使值等于默认 "ask"),用于压制 YOLO 隐式
                // 映射;非法值归一化为 "ask" 且不置标记。
                if (alj.contains("question_policy") && alj["question_policy"].is_string()) {
                    std::string qp = alj["question_policy"].get<std::string>();
                    if (qp == "ask" || qp == "deny" || qp == "timeout") {
                        cfg.agent_loop.question_policy = qp;
                        cfg.agent_loop.question_policy_explicit = true;
                    } else {
                        LOG_WARN("[config] agent_loop.question_policy=\"" + qp +
                                 "\" is invalid (expected ask|deny|timeout); using \"ask\"");
                        cfg.agent_loop.question_policy = "ask";
                    }
                }
                if (alj.contains("question_timeout_seconds") &&
                    alj["question_timeout_seconds"].is_number_integer()) {
                    int v = alj["question_timeout_seconds"].get<int>();
                    if (v < 5 || v > 3600) {
                        LOG_WARN("[config] agent_loop.question_timeout_seconds=" +
                                 std::to_string(v) +
                                 " is out of range [5, 3600]; using default 60");
                        v = 60;
                    }
                    cfg.agent_loop.question_timeout_seconds = v;
                }
                // 工具前言(add-tool-preamble):默认关闭;mode 非法归一化为
                // "prompt";sidecar_wait_ms clamp [0, 15000]。
                if (alj.contains("tool_preamble") && alj["tool_preamble"].is_object()) {
                    const auto& tpj = alj["tool_preamble"];
                    auto& tp = cfg.agent_loop.tool_preamble;
                    if (tpj.contains("enabled") && tpj["enabled"].is_boolean()) {
                        tp.enabled = tpj["enabled"].get<bool>();
                    }
                    if (tpj.contains("mode") && tpj["mode"].is_string()) {
                        const std::string mode = tpj["mode"].get<std::string>();
                        if (mode == "prompt" || mode == "reasoning" || mode == "sidecar") {
                            tp.mode = mode;
                        } else {
                            LOG_WARN("[config] agent_loop.tool_preamble.mode=\"" + mode +
                                     "\" is invalid (expected prompt|reasoning|sidecar); using \"prompt\"");
                            tp.mode = "prompt";
                        }
                    }
                    if (tpj.contains("sidecar_model") && tpj["sidecar_model"].is_string()) {
                        tp.sidecar_model = tpj["sidecar_model"].get<std::string>();
                    }
                    if (tpj.contains("sidecar_wait_ms") &&
                        tpj["sidecar_wait_ms"].is_number_integer()) {
                        int v = tpj["sidecar_wait_ms"].get<int>();
                        if (v < 0 || v > 15000) {
                            LOG_WARN("[config] agent_loop.tool_preamble.sidecar_wait_ms=" +
                                     std::to_string(v) +
                                     " is out of range [0, 15000]; clamping");
                            v = v < 0 ? 0 : 15000;
                        }
                        tp.sidecar_wait_ms = v;
                    }
                }
                if (alj.contains("jb_mode") && alj["jb_mode"].is_boolean()) {
                    cfg.agent_loop.jb_mode = alj["jb_mode"].get<bool>();
                }
                // Legacy keys (auto_continue, max_consecutive_empty_iterations)
                // from the just-rolled-back agentic-loop-terminator change are
                // silently ignored — see align-loop-with-hermes.
            }
            // --- model profiles (openspec/changes/model-profiles) ---
            // 缺失视为旧 schema,load_config 末尾会从 legacy provider 字段合成兜底。
            if (j.contains("saved_models")) {
                saved_models_key_present = true;
                std::string err;
                auto parsed = parse_saved_models(j["saved_models"], err);
                if (!parsed.has_value()) {
                    fatal_config_value("saved_models parse failure: " + err);
                }
                cfg.saved_models = std::move(*parsed);

                std::string repair_error;
                auto repairs = repair_duplicate_saved_model_names(
                    cfg.saved_models, repair_error);
                if (!repairs.has_value()) {
                    fatal_config_value(
                        "failed to repair duplicate saved model names: " +
                        repair_error);
                }
                if (!repairs->empty()) {
                    for (const auto& repair : *repairs) {
                        j["saved_models"][repair.index]["name"] =
                            repair.repaired_name;
                    }

                    // Windows may reject replacing a file that this process still
                    // has open, so close the startup read handle before atomic rename.
                    ifs.close();
                    const std::string repaired_bytes = j.dump(2) + "\n";
                    if (!atomic_write_file(config_path, repaired_bytes, true)) {
                        throw ConfigLoadFailure(
                            "repair_persistence",
                            "failed to persist repaired duplicate saved model names to " +
                            config_path);
                    }
                    active_bytes = repaired_bytes;
                    for (const auto& repair : *repairs) {
                        LOG_WARN(
                            "[config] repaired duplicate saved model name at index " +
                            std::to_string(repair.index) + ": " +
                            nlohmann::json(repair.original_name).dump() + " -> " +
                            nlohmann::json(repair.repaired_name).dump());
                    }
                }
            }
            if (j.contains("default_model_name") && j["default_model_name"].is_string()) {
                cfg.default_model_name = j["default_model_name"].get<std::string>();
            }
            if (!cfg.default_model_name.empty() &&
                !cfg.saved_models.empty() &&
                find_profile_by_name(cfg.saved_models, cfg.default_model_name) == nullptr) {
                const std::string stale_default_name = cfg.default_model_name;
                cfg.default_model_name = cfg.saved_models.front().name;
                j["default_model_name"] = cfg.default_model_name;

                // Keep this recoverable startup repair out of every UI surface.
                // Close the startup read handle before the atomic replace on Windows.
                ifs.close();
                Logger::instance().init_with_rotation_if_disabled(
                    get_logs_dir(), "config", /*mirror_stderr=*/false);
                const std::string repair_summary =
                    nlohmann::json(stale_default_name).dump() + " -> " +
                    nlohmann::json(cfg.default_model_name).dump();
                const std::string repaired_bytes = j.dump(2) + "\n";
                if (atomic_write_file(config_path, repaired_bytes, true)) {
                    active_bytes = repaired_bytes;
                    LOG_WARN(
                        "[config] repaired missing default model reference " +
                        repair_summary + "; persisted to " +
                        nlohmann::json(config_path).dump());
                } else {
                    // The in-memory fallback is safe for this run, but the
                    // still-stale disk bytes must never replace last-good.
                    active_bytes.reset();
                    LOG_ERROR(
                        "[config] repaired missing default model reference in memory " +
                        repair_summary + "; failed to persist to " +
                        nlohmann::json(config_path).dump() +
                        "; continuing startup and retrying on the next load");
                }
            }

            cfg.mcp_servers = parse_mcp_config(
                j.value("mcp_servers", nlohmann::json::object()));
        } catch (const nlohmann::json::parse_error& e) {
            throw ConfigLoadFailure(
                "json_parse",
                "config JSON parse failure at byte " +
                std::to_string(e.byte));
        } catch (const nlohmann::json::exception& e) {
            throw ConfigLoadFailure(
                "json_value",
                "config JSON value failure (error id " +
                std::to_string(e.id) + ")");
        }
    }

    // Prove the persisted JSON independently of runtime-only environment
    // overrides. Keep cfg unsynthesized until after overrides so legacy model
    // profiles continue to inherit environment-provided values.
    AppConfig persisted_cfg = cfg;
    synthesize_legacy_saved_model_if_needed(
        persisted_cfg, saved_models_key_present);
    if (!persisted_cfg.saved_models.empty()) {
        std::string err;
        if (!validate_saved_models(
                persisted_cfg.saved_models,
                persisted_cfg.default_model_name,
                err)) {
            fatal_config_value("saved_models validation failure: " + err);
        }
        sanitize_disabled_model_providers(persisted_cfg);
    } else if (!persisted_cfg.default_model_name.empty()) {
        LOG_WARN("[config] default_model_name ignored because saved_models is empty: " +
                 persisted_cfg.default_model_name);
        persisted_cfg.default_model_name.clear();
    }
    if (persisted_cfg.saved_models.empty() &&
        !persisted_cfg.provider.empty() &&
        !is_runtime_model_provider_enabled(persisted_cfg.provider)) {
        LOG_WARN("[config] provider '" + persisted_cfg.provider +
                 "' ignored because no enabled model profiles are configured");
        persisted_cfg.provider.clear();
    }

    const auto validation_errors = validate_config(persisted_cfg);
    if (!validation_errors.empty()) {
        fatal_config_value(
            "configuration validation failure: " +
            validation_errors.front() +
            (validation_errors.size() > 1
                ? " (and " +
                    std::to_string(validation_errors.size() - 1) +
                    " more)"
                : ""));
    }

    if (proven_persisted_bytes && active_bytes.has_value()) {
        *proven_persisted_bytes = active_bytes;
    }

    if (!apply_environment_overrides) {
        return persisted_cfg;
    }

    {
        // Environment variable overrides are runtime-only and are applied only
        // after the persisted JSON has proven valid. A bad environment value
        // must never cause rollback of a healthy config file.
        std::string env;
        if (getenv_utf8("ACECODE_PROVIDER", env)) {
            cfg.provider = env;
        }
        if (getenv_utf8("ACECODE_OPENAI_BASE_URL", env)) {
            cfg.openai.base_url = env;
        }
        if (getenv_utf8("ACECODE_OPENAI_API_KEY", env)) {
            cfg.openai.api_key = env;
        }
        if (getenv_utf8("ACECODE_OPENAI_STREAM_TIMEOUT_MS", env)) {
            auto parsed = parse_positive_int(env);
            if (parsed.has_value()) {
                cfg.openai.stream_timeout_ms = *parsed;
            } else {
                LOG_WARN("[config] ACECODE_OPENAI_STREAM_TIMEOUT_MS='" + env +
                         "' invalid; expected positive integer, keeping " +
                         std::to_string(cfg.openai.stream_timeout_ms));
            }
        }
        if (getenv_utf8("ACECODE_MODEL", env)) {
            if (cfg.provider == "openai") {
                cfg.openai.model = env;
            } else if (cfg.provider == "codex") {
                cfg.codex.model = env;
            } else {
                cfg.copilot.model = env;
            }
        }
        if (getenv_utf8("ACECODE_UPGRADE_BASE_URL", env)) {
            cfg.upgrade.base_url = normalize_upgrade_base_url(env);
        }
    }

    synthesize_legacy_saved_model_if_needed(cfg, saved_models_key_present);
    if (!cfg.saved_models.empty()) {
        std::string err;
        if (!validate_saved_models(cfg.saved_models, cfg.default_model_name, err)) {
            // The persisted equivalent was already validated above, so any new
            // failure at this point belongs to runtime environment overrides
            // and must not trigger a JSON rollback.
            fatal_runtime_config_value(
                "runtime saved_models validation failure: " + err);
        }
        sanitize_disabled_model_providers(cfg);
    } else if (!cfg.default_model_name.empty()) {
        LOG_WARN("[config] default_model_name ignored because saved_models is empty: " +
                 cfg.default_model_name);
        cfg.default_model_name.clear();
    }
    if (cfg.saved_models.empty() &&
        !cfg.provider.empty() &&
        !is_runtime_model_provider_enabled(cfg.provider)) {
        LOG_WARN("[config] provider '" + cfg.provider +
                 "' ignored because no enabled model profiles are configured");
        cfg.provider.clear();
    }

    return cfg;
}

namespace {

std::optional<std::string> read_config_bytes_for_recovery(
    const std::string& path,
    std::string* error = nullptr) {
    if (error) error->clear();
    std::ifstream input(path_from_utf8(path), std::ios::binary);
    if (!input.is_open()) {
        if (error) *error = "failed to open config file: " + path;
        return std::nullopt;
    }
    std::ostringstream bytes;
    bytes << input.rdbuf();
    if (input.bad()) {
        if (error) *error = "failed to read config file: " + path;
        return std::nullopt;
    }
    return bytes.str();
}

void init_config_recovery_logging() {
    Logger::instance().init_with_rotation_if_disabled(
        get_logs_dir(), "config", /*mirror_stderr=*/false);
}

void snapshot_valid_config(const std::string& config_path,
                           const std::string& proven_bytes) {
    // Avoid rewriting a credentials-bearing snapshot on every read-only load.
    auto existing = read_last_good_config(config_path);
    if (existing.has_value() && *existing == proven_bytes) return;

    std::string snapshot_error;
    if (!write_last_good_config(
            config_path, proven_bytes, &snapshot_error)) {
        init_config_recovery_logging();
        LOG_ERROR("[config_recovery] " + snapshot_error);
    }
}

[[noreturn]] void terminate_config_load(
    const std::string& config_path,
    const ConfigLoadFailure& failure,
    const std::string& recovery_summary = {}) {
    init_config_recovery_logging();
    std::string message = "category=" + failure.category();
    if (!recovery_summary.empty()) {
        message += "; rollback=" + recovery_summary;
    }
    LOG_ERROR("[config_recovery] unrecoverable config load: path=" +
              nlohmann::json(config_path).dump() + "; " + message);
    std::cerr << "[config] fatal: " << failure.what();
    if (!recovery_summary.empty()) {
        std::cerr << "; automatic rollback failed: " << recovery_summary;
    }
    std::cerr << std::endl;
    std::exit(1);
}

class StagedConfigCleanup {
public:
    explicit StagedConfigCleanup(std::string path) : path_(std::move(path)) {}
    ~StagedConfigCleanup() {
        std::error_code ec;
        fs::remove(path_from_utf8(path_), ec);
        fs::remove(path_from_utf8(path_ + ".tmp"), ec);
    }

private:
    std::string path_;
};

} // namespace

AppConfig load_config_from_path(
    const std::string& explicit_path,
    bool apply_environment_overrides) {
    const std::string config_path =
        path_to_utf8(path_from_utf8(explicit_path));

    try {
        std::optional<std::string> proven_bytes;
        AppConfig cfg = load_config_from_path_once(
            config_path, apply_environment_overrides, &proven_bytes);
        if (proven_bytes.has_value()) {
            snapshot_valid_config(config_path, *proven_bytes);
        }
        return cfg;
    } catch (const ConfigLoadFailure& initial_failure) {
        if (initial_failure.category() == "filesystem_read" ||
            initial_failure.category() == "repair_persistence") {
            terminate_config_load(config_path, initial_failure);
        }

        init_config_recovery_logging();
        LOG_ERROR("[config_recovery] active config rejected: path=" +
                  nlohmann::json(config_path).dump() +
                  "; category=" + initial_failure.category());

        std::string read_error;
        auto invalid_bytes = read_config_bytes_for_recovery(
            config_path, &read_error);
        if (!invalid_bytes.has_value()) {
            terminate_config_load(
                config_path, initial_failure, "active file capture failed");
        }

        std::string recovery_error;
        auto last_good_bytes = read_last_good_config(
            config_path, &recovery_error);
        if (!last_good_bytes.has_value()) {
            terminate_config_load(
                config_path, initial_failure,
                "no readable last-good snapshot");
        }

        auto staged_path = stage_config_recovery_candidate(
            config_path, *last_good_bytes, &recovery_error);
        if (!staged_path.has_value()) {
            terminate_config_load(
                config_path, initial_failure,
                "last-good staging failed");
        }
        StagedConfigCleanup staged_cleanup(*staged_path);

        std::optional<std::string> restored_bytes;
        try {
            (void)load_config_from_path_once(
                *staged_path,
                /*apply_environment_overrides=*/false,
                &restored_bytes);
        } catch (const ConfigLoadFailure& candidate_failure) {
            terminate_config_load(
                config_path, initial_failure,
                "last-good snapshot rejected (" +
                candidate_failure.category() + ")");
        }

        if (!restored_bytes.has_value()) {
            terminate_config_load(
                config_path, initial_failure,
                "validated snapshot produced no persistent bytes");
        }

        auto invalid_backup_path = archive_invalid_config(
            config_path, *invalid_bytes, &recovery_error);
        if (!invalid_backup_path.has_value()) {
            terminate_config_load(
                config_path, initial_failure,
                "invalid-file archival failed");
        }

        // Another process may have completed the same recovery while this one
        // validated the snapshot. Accept its proven-valid active file instead
        // of replacing it again; the captured invalid bytes remain archived.
        auto latest_bytes = read_config_bytes_for_recovery(config_path);
        if (latest_bytes.has_value() && *latest_bytes != *invalid_bytes) {
            try {
                std::optional<std::string> concurrent_bytes;
                AppConfig concurrent = load_config_from_path_once(
                    config_path,
                    apply_environment_overrides,
                    &concurrent_bytes);
                if (concurrent_bytes.has_value()) {
                    snapshot_valid_config(config_path, *concurrent_bytes);
                    LOG_WARN(
                        "[config_recovery] concurrent process restored config: path=" +
                        nlohmann::json(config_path).dump() +
                        "; invalid_backup=" +
                        nlohmann::json(*invalid_backup_path).dump());
                    return concurrent;
                }
            } catch (const ConfigLoadFailure&) {
                // The replacement is also invalid; continue with the already
                // validated last-good candidate below.
            }
        }

        if (!atomic_write_file(config_path, *restored_bytes, true)) {
            terminate_config_load(
                config_path, initial_failure,
                "atomic last-good restore failed");
        }

        AppConfig recovered;
        std::optional<std::string> recovered_bytes;
        try {
            recovered = load_config_from_path_once(
                config_path,
                apply_environment_overrides,
                &recovered_bytes);
        } catch (const ConfigLoadFailure& retry_failure) {
            // Preserve the original active failure for manual correction if an
            // unexpected path-dependent check rejects the staged candidate.
            (void)atomic_write_file(config_path, *invalid_bytes, true);
            terminate_config_load(
                config_path, initial_failure,
                "restored config retry failed (" +
                retry_failure.category() + ")");
        }
        if (!recovered_bytes.has_value()) {
            (void)atomic_write_file(config_path, *invalid_bytes, true);
            terminate_config_load(
                config_path, initial_failure,
                "restored config produced no persistent bytes");
        }

        std::string notice_error;
        if (!write_config_recovery_notice(
                config_path, *invalid_backup_path, &notice_error)) {
            LOG_ERROR("[config_recovery] rollback completed but recovery "
                      "notice persistence failed: path=" +
                      nlohmann::json(config_path).dump());
        }
        snapshot_valid_config(config_path, *recovered_bytes);
        LOG_WARN("[config_recovery] restored last-good config: path=" +
                 nlohmann::json(config_path).dump() +
                 "; last_good=" +
                 nlohmann::json(
                     config_recovery_paths(config_path).last_good_path).dump() +
                 "; invalid_backup=" +
                 nlohmann::json(*invalid_backup_path).dump() +
                 "; notice_persisted=" +
                 std::string(notice_error.empty() ? "true" : "false"));
        return recovered;
    }
}

bool was_acecode_home_created_by_process() {
    return g_acecode_home_created_by_process.load();
}

bool consume_acecode_home_created_by_process() {
    return g_acecode_home_created_by_process.exchange(false);
}

void reset_acecode_home_created_flag_for_test() {
    g_acecode_home_created_by_process.store(false);
}

namespace {

nlohmann::json build_config_json(const AppConfig& cfg) {
    nlohmann::json j;
    j["provider"] = cfg.provider;
    j["openai"]["base_url"] = cfg.openai.base_url;
    j["openai"]["api_key"] = cfg.openai.api_key;
    j["openai"]["model"] = cfg.openai.model;
    if (cfg.openai.stream_timeout_ms != OpenAiConfig::kDefaultStreamTimeoutMs) {
        j["openai"]["stream_timeout_ms"] = cfg.openai.stream_timeout_ms;
    }
    if (cfg.openai.models_dev_provider_id.has_value() &&
        !cfg.openai.models_dev_provider_id->empty()) {
        j["openai"]["models_dev_provider_id"] = *cfg.openai.models_dev_provider_id;
    }
    if (!cfg.openai.request_headers.empty()) {
        nlohmann::json headers = nlohmann::json::object();
        for (const auto& [k, v] : cfg.openai.request_headers) headers[k] = v;
        j["openai"]["request_headers"] = std::move(headers);
    }
    j["copilot"]["model"] = cfg.copilot.model;
    j["codex"]["model"] = cfg.codex.model;
    j["context_window"] = cfg.context_window;
    j["max_sessions"] = cfg.max_sessions;
    if (cfg.task_suggestion_compact_threshold != 3) {
        j["task_suggestion_compact_threshold"] = cfg.task_suggestion_compact_threshold;
    }
    if (normalize_permission_mode_name(cfg.default_permission_mode) != "default") {
        j["default_permission_mode"] =
            normalize_permission_mode_name(cfg.default_permission_mode);
    }

    if (cfg.sandbox_disable_migration_completed) {
        j["migrations"]["disable_sandbox_once"] = true;
    }
    {
        SandboxConfig sandbox_d;
        nlohmann::json sbj = nlohmann::json::object();
        if (cfg.sandbox.enabled != sandbox_d.enabled) sbj["enabled"] = cfg.sandbox.enabled;
        if (cfg.sandbox.network_access != sandbox_d.network_access)
            sbj["network_access"] = cfg.sandbox.network_access;
        if (cfg.sandbox.exclude_tmpdir != sandbox_d.exclude_tmpdir)
            sbj["exclude_tmpdir"] = cfg.sandbox.exclude_tmpdir;
        if (!cfg.sandbox.writable_roots.empty())
            sbj["writable_roots"] = cfg.sandbox.writable_roots;
        {
            nlohmann::json fsj = nlohmann::json::object();
            if (!cfg.sandbox.filesystem_read.empty()) fsj["read"] = cfg.sandbox.filesystem_read;
            if (!cfg.sandbox.filesystem_write.empty()) fsj["write"] = cfg.sandbox.filesystem_write;
            if (!cfg.sandbox.filesystem_deny.empty()) fsj["deny"] = cfg.sandbox.filesystem_deny;
            if (!fsj.empty()) sbj["filesystem"] = fsj;
        }
        if (cfg.sandbox.deny_defaults != sandbox_d.deny_defaults)
            sbj["deny_defaults"] = cfg.sandbox.deny_defaults;
        if (!cfg.sandbox.windows_backend.empty() && cfg.sandbox.windows_backend != "restricted-token")
            sbj["windows_backend"] = cfg.sandbox.windows_backend;
        if (!sbj.empty()) j["sandbox"] = sbj;
    }

    SkillsConfig skills_d;
    if (!cfg.skills.disabled.empty() ||
        !cfg.skills.external_dirs.empty() ||
        cfg.skills.reuse_opencode != skills_d.reuse_opencode ||
        cfg.skills.idle_days != skills_d.idle_days) {
        nlohmann::json sj = nlohmann::json::object();
        if (!cfg.skills.disabled.empty()) sj["disabled"] = cfg.skills.disabled;
        if (!cfg.skills.external_dirs.empty()) sj["external_dirs"] = cfg.skills.external_dirs;
        if (cfg.skills.reuse_opencode != skills_d.reuse_opencode)
            sj["reuse_opencode"] = cfg.skills.reuse_opencode;
        if (cfg.skills.idle_days != skills_d.idle_days)
            sj["idle_days"] = cfg.skills.idle_days;
        j["skills"] = sj;
    }

    {
        // Persist daemon and web sections so users discover the available
        // tunables; emit only fields that diverge from defaults to keep
        // the file readable.
        DaemonConfig dd;
        nlohmann::json dj = nlohmann::json::object();
        if (cfg.daemon.auto_start_on_double_click != dd.auto_start_on_double_click)
            dj["auto_start_on_double_click"] = cfg.daemon.auto_start_on_double_click;
        if (cfg.daemon.service_name != dd.service_name)
            dj["service_name"] = cfg.daemon.service_name;
        if (cfg.daemon.heartbeat_interval_ms != dd.heartbeat_interval_ms)
            dj["heartbeat_interval_ms"] = cfg.daemon.heartbeat_interval_ms;
        if (cfg.daemon.heartbeat_timeout_ms != dd.heartbeat_timeout_ms)
            dj["heartbeat_timeout_ms"] = cfg.daemon.heartbeat_timeout_ms;
        if (!dj.empty()) j["daemon"] = dj;

        WebConfig wd;
        nlohmann::json wj = nlohmann::json::object();
        if (cfg.web.enabled != wd.enabled)
            wj["enabled"] = cfg.web.enabled;
        // web.bind is a legacy input only. New writes rely on the canonical
        // 127.0.0.1 default and persist remote proxy intent separately.
        if (cfg.web.port != wd.port)
            wj["port"] = cfg.web.port;
        if (cfg.web.remote_enabled != wd.remote_enabled)
            wj["remote_enabled"] = cfg.web.remote_enabled;
        if (cfg.web.remote_port != wd.remote_port)
            wj["remote_port"] = cfg.web.remote_port;
        if (!cfg.web.static_dir.empty())
            wj["static_dir"] = cfg.web.static_dir;
        if (!wj.empty()) j["web"] = wj;

        WebUiPreferencesConfig web_ui_d;
        nlohmann::json web_uij = nlohmann::json::object();
        if (cfg.web_ui.theme != web_ui_d.theme)
            web_uij["theme"] = cfg.web_ui.theme;
        if (cfg.web_ui.color_theme != web_ui_d.color_theme)
            web_uij["color_theme"] = cfg.web_ui.color_theme;
        if (cfg.web_ui.font_size != web_ui_d.font_size)
            web_uij["font_size"] = cfg.web_ui.font_size;
        if (cfg.web_ui.sidebar_session_time != web_ui_d.sidebar_session_time)
            web_uij["sidebar_session_time"] = cfg.web_ui.sidebar_session_time;
        if (cfg.web_ui.message_auto_collapse != web_ui_d.message_auto_collapse)
            web_uij["message_auto_collapse"] = cfg.web_ui.message_auto_collapse;
        if (!web_uij.empty()) j["web_ui"] = std::move(web_uij);

        MemoryConfig mem_d;
        nlohmann::json memj = nlohmann::json::object();
        if (cfg.memory.enabled != mem_d.enabled)
            memj["enabled"] = cfg.memory.enabled;
        if (cfg.memory.max_index_bytes != mem_d.max_index_bytes)
            memj["max_index_bytes"] = cfg.memory.max_index_bytes;
        if (!memj.empty()) j["memory"] = memj;

        ProjectInstructionsConfig pi_d;
        nlohmann::json pij = nlohmann::json::object();
        if (cfg.project_instructions.enabled != pi_d.enabled)
            pij["enabled"] = cfg.project_instructions.enabled;
        if (cfg.project_instructions.max_depth != pi_d.max_depth)
            pij["max_depth"] = cfg.project_instructions.max_depth;
        if (cfg.project_instructions.max_bytes != pi_d.max_bytes)
            pij["max_bytes"] = cfg.project_instructions.max_bytes;
        if (cfg.project_instructions.max_total_bytes != pi_d.max_total_bytes)
            pij["max_total_bytes"] = cfg.project_instructions.max_total_bytes;
        if (cfg.project_instructions.filenames != pi_d.filenames)
            pij["filenames"] = cfg.project_instructions.filenames;
        if (cfg.project_instructions.read_claude_md != pi_d.read_claude_md)
            pij["read_claude_md"] = cfg.project_instructions.read_claude_md;
        if (!pij.empty()) j["project_instructions"] = pij;

        CustomInstructionsConfig ci_d;
        nlohmann::json cij = nlohmann::json::object();
        const std::string custom_text = cfg.custom_instructions.text_snapshot();
        if (custom_text != ci_d.text)
            cij["text"] = custom_text;
        if (!cij.empty()) j["custom_instructions"] = cij;

        if (!cfg.connectors.empty()) {
            j["connectors"] = connectors_to_json(cfg.connectors);
        }

        ModelsDevConfig md;
        nlohmann::json mdj = nlohmann::json::object();
        if (cfg.models_dev.allow_network != md.allow_network)
            mdj["allow_network"] = cfg.models_dev.allow_network;
        if (cfg.models_dev.refresh_on_command_only != md.refresh_on_command_only)
            mdj["refresh_on_command_only"] = cfg.models_dev.refresh_on_command_only;
        if (cfg.models_dev.user_override_path.has_value() &&
            !cfg.models_dev.user_override_path->empty())
            mdj["user_override_path"] = *cfg.models_dev.user_override_path;
        if (!mdj.empty()) j["models_dev"] = mdj;

        InputHistoryConfig ih_d;
        nlohmann::json ihj = nlohmann::json::object();
        if (cfg.input_history.enabled != ih_d.enabled)
            ihj["enabled"] = cfg.input_history.enabled;
        if (cfg.input_history.max_entries != ih_d.max_entries)
            ihj["max_entries"] = cfg.input_history.max_entries;
        if (!ihj.empty()) j["input_history"] = ihj;

        AgentLoopConfig al_d;
        nlohmann::json alj = nlohmann::json::object();
        if (cfg.agent_loop.max_iterations != al_d.max_iterations)
            alj["max_iterations"] = cfg.agent_loop.max_iterations;
        // question_policy_explicit 是运行时标记,永不序列化。CLI 覆盖也只改
        // 内存不落盘,这里只在配置值本身偏离默认时写出。
        if (cfg.agent_loop.question_policy != al_d.question_policy)
            alj["question_policy"] = cfg.agent_loop.question_policy;
        if (cfg.agent_loop.question_timeout_seconds != al_d.question_timeout_seconds)
            alj["question_timeout_seconds"] = cfg.agent_loop.question_timeout_seconds;
        {
            const ToolPreambleConfig tp_d;
            const auto& tp = cfg.agent_loop.tool_preamble;
            nlohmann::json tpj = nlohmann::json::object();
            if (tp.enabled != tp_d.enabled) tpj["enabled"] = tp.enabled;
            if (tp.mode != tp_d.mode) tpj["mode"] = tp.mode;
            if (tp.sidecar_model != tp_d.sidecar_model) tpj["sidecar_model"] = tp.sidecar_model;
            if (tp.sidecar_wait_ms != tp_d.sidecar_wait_ms) tpj["sidecar_wait_ms"] = tp.sidecar_wait_ms;
            if (!tpj.empty()) alj["tool_preamble"] = tpj;
        }
        if (cfg.agent_loop.jb_mode != al_d.jb_mode)
            alj["jb_mode"] = cfg.agent_loop.jb_mode;
        if (!alj.empty()) j["agent_loop"] = alj;

        AskConfig ask_d;
        nlohmann::json askj = nlohmann::json::object();
        if (cfg.ask.max_questions != ask_d.max_questions)
            askj["max_questions"] = cfg.ask.max_questions;
        if (cfg.ask.max_options != ask_d.max_options)
            askj["max_options"] = cfg.ask.max_options;
        if (!askj.empty()) j["ask"] = askj;

        TuiConfig tui_d;
        nlohmann::json tj = nlohmann::json::object();
        if (cfg.tui.alt_screen_mode != tui_d.alt_screen_mode)
            tj["alt_screen_mode"] = cfg.tui.alt_screen_mode;
        if (cfg.tui.sync_output_mode != tui_d.sync_output_mode)
            tj["sync_output_mode"] = cfg.tui.sync_output_mode;
        if (cfg.tui.page_keys_single_line != tui_d.page_keys_single_line)
            tj["page_keys_single_line"] = cfg.tui.page_keys_single_line;
        if (cfg.tui.theme != tui_d.theme)
            tj["theme"] = cfg.tui.theme;
        if (cfg.tui.question_min_visible_rows != tui_d.question_min_visible_rows) {
            tj["question_min_visible_rows"] = cfg.tui.question_min_visible_rows;
        }
        if (cfg.tui.question_selection_feedback_ms !=
            tui_d.question_selection_feedback_ms) {
            tj["question_selection_feedback_ms"] =
                cfg.tui.question_selection_feedback_ms;
        }
        if (!tj.empty()) j["tui"] = tj;

        DesktopConfig desk_d;
        DesktopNotificationsConfig dn_d;
        nlohmann::json dnj = nlohmann::json::object();
        if (cfg.desktop.notifications.enabled != dn_d.enabled)
            dnj["enabled"] = cfg.desktop.notifications.enabled;
        if (cfg.desktop.notifications.on_permission != dn_d.on_permission)
            dnj["on_permission"] = cfg.desktop.notifications.on_permission;
        if (cfg.desktop.notifications.on_question != dn_d.on_question)
            dnj["on_question"] = cfg.desktop.notifications.on_question;
        if (cfg.desktop.notifications.on_completion != dn_d.on_completion)
            dnj["on_completion"] = cfg.desktop.notifications.on_completion;
        if (cfg.desktop.notifications.suppress_when_focused != dn_d.suppress_when_focused)
            dnj["suppress_when_focused"] = cfg.desktop.notifications.suppress_when_focused;
        nlohmann::json deskj = nlohmann::json::object();
        if (cfg.desktop.close_to_tray != desk_d.close_to_tray)
            deskj["close_to_tray"] = cfg.desktop.close_to_tray;
        if (cfg.desktop.allow_multiple_instances != desk_d.allow_multiple_instances)
            deskj["allow_multiple_instances"] = cfg.desktop.allow_multiple_instances;
        if (cfg.desktop.close_behavior != desk_d.close_behavior) {
            deskj["close_behavior"] = std::string(
                desktop_close_behavior_value(cfg.desktop.close_behavior));
        }
        if (cfg.desktop.continue_background_process !=
            desk_d.continue_background_process) {
            deskj["continue_background_process"] =
                cfg.desktop.continue_background_process;
        }
        if (!dnj.empty()) {
            deskj["notifications"] = dnj;
        }
        // console:schema sparse — 只有非空字段才落盘。legacy git_bash_path 不再写出,
        // 它已在加载时并入 shell_paths["git-bash"]。
        {
            nlohmann::json cj = nlohmann::json::object();
            if (!cfg.console.shell.empty()) cj["shell"] = cfg.console.shell;
            if (!cfg.console.default_shell.empty()) cj["default_shell"] = cfg.console.default_shell;
            nlohmann::json spj = nlohmann::json::object();
            for (const auto& [id, path] : cfg.console.shell_paths) {
                if (!id.empty() && !path.empty()) spj[id] = path;
            }
            if (!spj.empty()) cj["shell_paths"] = std::move(spj);
            if (!cj.empty()) j["console"] = std::move(cj);
        }
        // toolchains:同样 sparse,空目录不落盘。
        {
            nlohmann::json tj = nlohmann::json::object();
            if (!cfg.toolchains.python.empty()) tj["python"] = cfg.toolchains.python;
            if (!cfg.toolchains.node.empty()) tj["node"] = cfg.toolchains.node;
            if (!cfg.toolchains.csharp.empty()) tj["csharp"] = cfg.toolchains.csharp;
            if (!tj.empty()) j["toolchains"] = std::move(tj);
        }
        if (!deskj.empty()) {
            j["desktop"] = deskj;
        }

        UiConfig ui_d;
        if (cfg.ui.locale != ui_d.locale) {
            j["ui"]["locale"] = cfg.ui.locale;
        }

        const auto pointer_color = computer_use::pointer_appearance::normalize_color(cfg.computer_use.pointer_color);
        if (!computer_use::pointer_appearance::valid_style(cfg.computer_use.pointer_style) || !pointer_color)
            throw std::runtime_error("refusing to save invalid computer_use pointer appearance");
        if (cfg.computer_use.enabled || cfg.computer_use.pointer_style != computer_use::pointer_appearance::kDefaultStyle
            || *pointer_color != computer_use::pointer_appearance::kDefaultColor) {
            j["computer_use"] = {{"enabled", cfg.computer_use.enabled},
                {"pointer_style", cfg.computer_use.pointer_style}, {"pointer_color", *pointer_color}};
        }

        nlohmann::json summary = nlohmann::json::object();
        if (cfg.summary_generation.enabled)
            summary["enabled"] = true;
        if (!cfg.summary_generation.model_name.empty())
            summary["model_name"] = cfg.summary_generation.model_name;
        if (!summary.empty()) j["summary_generation"] = std::move(summary);

        SessionTitleConfig st_d;
        nlohmann::json stj = nlohmann::json::object();
        if (cfg.session_title.enabled != st_d.enabled)
            stj["enabled"] = cfg.session_title.enabled;
        if (cfg.session_title.model_name != st_d.model_name)
            stj["model_name"] = cfg.session_title.model_name;
        if (cfg.session_title.max_input_bytes != st_d.max_input_bytes)
            stj["max_input_bytes"] = cfg.session_title.max_input_bytes;
        if (cfg.session_title.timeout_ms != st_d.timeout_ms)
            stj["timeout_ms"] = cfg.session_title.timeout_ms;
        if (!stj.empty()) j["session_title"] = stj;

        NetworkConfig net_d;
        nlohmann::json nj = nlohmann::json::object();
        if (cfg.network.proxy_mode != net_d.proxy_mode)
            nj["proxy_mode"] = cfg.network.proxy_mode;
        if (cfg.network.proxy_url != net_d.proxy_url)
            nj["proxy_url"] = cfg.network.proxy_url;
        if (cfg.network.proxy_no_proxy != net_d.proxy_no_proxy)
            nj["proxy_no_proxy"] = cfg.network.proxy_no_proxy;
        if (cfg.network.proxy_probe_enabled != net_d.proxy_probe_enabled)
            nj["proxy_probe_enabled"] = cfg.network.proxy_probe_enabled;
        if (cfg.network.proxy_probe_timeout_ms != net_d.proxy_probe_timeout_ms)
            nj["proxy_probe_timeout_ms"] = cfg.network.proxy_probe_timeout_ms;
        if (!nj.empty()) j["network"] = nj;

        FeaturesConfig features_d;
        nlohmann::json featuresj = nlohmann::json::object();
        if (cfg.features.hooks != features_d.hooks)
            featuresj["hooks"] = cfg.features.hooks;
        if (cfg.features.completed_turn_self_heal != features_d.completed_turn_self_heal)
            featuresj["completed_turn_self_heal"] = cfg.features.completed_turn_self_heal;
        if (!featuresj.empty()) j["features"] = featuresj;

        WebSearchConfig ws_d;
        nlohmann::json wsj = nlohmann::json::object();
        if (!utils::is_valid_http_base_url(cfg.web_search.rss_base_url)) {
            throw std::runtime_error(
                "web_search.rss_base_url must be HTTPS (or loopback HTTP) without "
                "credentials, query, or fragment");
        }
        if (cfg.web_search.enabled != ws_d.enabled)
            wsj["enabled"] = cfg.web_search.enabled;
        if (cfg.web_search.backend != ws_d.backend)
            wsj["backend"] = cfg.web_search.backend;
        if (cfg.web_search.api_key != ws_d.api_key)
            wsj["api_key"] = cfg.web_search.api_key;
        if (cfg.web_search.rss_base_url != ws_d.rss_base_url)
            wsj["rss_base_url"] = cfg.web_search.rss_base_url;
        if (cfg.web_search.max_results != ws_d.max_results)
            wsj["max_results"] = cfg.web_search.max_results;
        if (cfg.web_search.timeout_ms != ws_d.timeout_ms)
            wsj["timeout_ms"] = cfg.web_search.timeout_ms;
        if (!wsj.empty()) j["web_search"] = wsj;

        ImageGenerationConfig ig_d;
        nlohmann::json igj = nlohmann::json::object();
        if (!cfg.image_generation.base_url.empty() &&
            !utils::is_valid_http_base_url(cfg.image_generation.base_url)) {
            throw std::runtime_error(
                "image_generation.base_url must be HTTPS (or loopback HTTP) without "
                "credentials, query, or fragment");
        }
        if (cfg.image_generation.enabled != ig_d.enabled)
            igj["enabled"] = cfg.image_generation.enabled;
        if (cfg.image_generation.source != ig_d.source)
            igj["source"] = cfg.image_generation.source;
        if (cfg.image_generation.saved_model_name != ig_d.saved_model_name)
            igj["saved_model_name"] = cfg.image_generation.saved_model_name;
        if (cfg.image_generation.base_url != ig_d.base_url)
            igj["base_url"] = cfg.image_generation.base_url;
        if (cfg.image_generation.api_key != ig_d.api_key)
            igj["api_key"] = cfg.image_generation.api_key;
        {
            nlohmann::json mj = nlohmann::json::object();
            if (cfg.image_generation.model_standard != ig_d.model_standard)
                mj["standard"] = cfg.image_generation.model_standard;
            if (cfg.image_generation.model_high != ig_d.model_high)
                mj["high"] = cfg.image_generation.model_high;
            if (cfg.image_generation.model_ultra != ig_d.model_ultra)
                mj["ultra"] = cfg.image_generation.model_ultra;
            if (!mj.empty()) igj["models"] = mj;
        }
        if (cfg.image_generation.default_quality != ig_d.default_quality)
            igj["default_quality"] = cfg.image_generation.default_quality;
        if (cfg.image_generation.timeout_ms != ig_d.timeout_ms)
            igj["timeout_ms"] = cfg.image_generation.timeout_ms;
        if (!igj.empty()) j["image_generation"] = igj;

        nlohmann::json lspj = nlohmann::json::object();
        if (!cfg.lsp.enabled) lspj["enabled"] = false;
        if (!cfg.lsp.servers.empty()) {
            nlohmann::json serversj = nlohmann::json::object();
            for (const auto& [name, entry] : cfg.lsp.servers) {
                nlohmann::json sj = nlohmann::json::object();
                if (entry.disabled) sj["disabled"] = true;
                if (!entry.command.empty()) sj["command"] = entry.command;
                if (!entry.extensions.empty()) sj["extensions"] = entry.extensions;
                if (!entry.env.empty()) sj["env"] = entry.env;
                if (entry.initialization.is_object() && !entry.initialization.empty())
                    sj["initialization"] = entry.initialization;
                serversj[name] = sj;
            }
            lspj["servers"] = serversj;
        }
        if (!lspj.empty()) j["lsp"] = lspj;

        nlohmann::json wtj = nlohmann::json::object();
        if (!cfg.worktree.symlink_directories.empty())
            wtj["symlink_directories"] = cfg.worktree.symlink_directories;
        if (!cfg.worktree.sparse_paths.empty())
            wtj["sparse_paths"] = cfg.worktree.sparse_paths;
        if (!wtj.empty()) j["worktree"] = wtj;

        GitContextConfig gc_d;
        nlohmann::json gcj = nlohmann::json::object();
        if (cfg.git_context.enabled != gc_d.enabled)
            gcj["enabled"] = cfg.git_context.enabled;
        if (cfg.git_context.timeout_ms != gc_d.timeout_ms)
            gcj["timeout_ms"] = cfg.git_context.timeout_ms;
        if (!gcj.empty()) j["git_context"] = gcj;

        RemoteControlConfig rc_d;
        nlohmann::json rcj = nlohmann::json::object();
        if (cfg.remote_control.port != rc_d.port)
            rcj["port"] = cfg.remote_control.port;
        if (cfg.remote_control.token != rc_d.token)
            rcj["token"] = cfg.remote_control.token;
        if (cfg.remote_control.outbound_url != rc_d.outbound_url)
            rcj["outbound_url"] = cfg.remote_control.outbound_url;
        if (cfg.remote_control.default_channel != rc_d.default_channel)
            rcj["default_channel"] = cfg.remote_control.default_channel;
        if (cfg.remote_control.bound_session_id != rc_d.bound_session_id)
            rcj["bound_session_id"] = cfg.remote_control.bound_session_id;
        if (!cfg.remote_control.channels.empty()) {
            nlohmann::json channels = nlohmann::json::object();
            for (const auto& [name, channel] : cfg.remote_control.channels) {
                RemoteControlConfig::ChannelPluginConfig channel_d;
                nlohmann::json cj = nlohmann::json::object();
                if (channel.manifest_path != channel_d.manifest_path)
                    cj["manifest_path"] = channel.manifest_path;
                if (channel.timeout_ms != channel_d.timeout_ms)
                    cj["timeout_ms"] = channel.timeout_ms;
                if (channel.settings.is_object() && !channel.settings.empty())
                    cj["settings"] = channel.settings;
                channels[name] = std::move(cj);
            }
            rcj["channels"] = std::move(channels);
        }
        if (!rcj.empty()) j["remote_control"] = rcj;

        UpgradeConfig up_d;
        nlohmann::json upj = nlohmann::json::object();
        if (normalize_upgrade_base_url(cfg.upgrade.base_url) != up_d.base_url)
            upj["base_url"] = normalize_upgrade_base_url(cfg.upgrade.base_url);
        if (cfg.upgrade.timeout_ms != up_d.timeout_ms)
            upj["timeout_ms"] = cfg.upgrade.timeout_ms;
        if (!upj.empty()) j["upgrade"] = upj;
    }

    // --- model profiles ---
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : cfg.saved_models) {
        nlohmann::json ej = nlohmann::json::object();
        ej["name"] = e.name;
        ej["provider"] = e.provider;
        ej["model"] = e.model;
        if (!e.base_url.empty()) ej["base_url"] = e.base_url;
        if (!e.api_key.empty()) ej["api_key"] = e.api_key;
        if (e.models_dev_provider_id.has_value() && !e.models_dev_provider_id->empty()) {
            ej["models_dev_provider_id"] = *e.models_dev_provider_id;
        }
        if (e.context_window.has_value() && *e.context_window > 0) {
            ej["context_window"] = *e.context_window;
        }
        if (e.stream_timeout_ms.has_value() && *e.stream_timeout_ms > 0) {
            ej["stream_timeout_ms"] = *e.stream_timeout_ms;
        }
        if (!e.capabilities.empty()) {
            ej["capabilities"] = e.capabilities;
        }
        if (e.endpoint_mode.has_value()) {
            ej["endpoint_mode"] = *e.endpoint_mode;
        }
        if (e.max_output_tokens.has_value()) {
            ej["max_output_tokens"] = *e.max_output_tokens;
        }
        if (e.capabilities_source.has_value()) {
            ej["capabilities_source"] = *e.capabilities_source;
        }
        if (e.reasoning.has_value()) {
            ej["reasoning"] = model_reasoning_options_to_json(*e.reasoning);
        }
        if (!e.request_headers.empty()) {
            nlohmann::json headers = nlohmann::json::object();
            for (const auto& [k, v] : e.request_headers) headers[k] = v;
            ej["request_headers"] = std::move(headers);
        }
        if (e.readonly) ej["readonly"] = true;
        arr.push_back(std::move(ej));
    }
    j["saved_models"] = std::move(arr);
    if (!cfg.default_model_name.empty()) {
        j["default_model_name"] = cfg.default_model_name;
    }

    if (!cfg.mcp_servers.empty()) {
        j["mcp_servers"] = serialize_mcp_config(cfg.mcp_servers);
    }

    return j;
}

std::string serialize_valid_config(const AppConfig& cfg) {
    require_valid_mcp_config(serialize_mcp_config(cfg.mcp_servers));
    const auto validation_errors = validate_config(cfg);
    if (!validation_errors.empty()) {
        throw std::runtime_error(
            "refusing to save invalid config: " +
            validation_errors.front());
    }
    if (!cfg.saved_models.empty()) {
        std::string saved_models_error;
        if (!validate_saved_models(
                cfg.saved_models,
                cfg.default_model_name,
                saved_models_error)) {
            throw std::runtime_error(
                "refusing to save invalid config: " + saved_models_error);
        }
    }
    return build_config_json(cfg).dump(2) + "\n";
}

} // namespace

void save_config(const AppConfig& cfg) {
    std::string acecode_dir = get_acecode_dir();
    std::string config_path = path_to_utf8(path_from_utf8(acecode_dir) / "config.json");

    fs::path native_acecode_dir = path_from_utf8(acecode_dir);
    if (!fs::exists(native_acecode_dir)) {
        fs::create_directories(native_acecode_dir);
    }

    const std::string bytes = serialize_valid_config(cfg);
    write_validated_config_file(
        config_path, bytes, serialize_mcp_config(cfg.mcp_servers));
    std::string snapshot_error;
    if (!write_last_good_config(config_path, bytes, &snapshot_error)) {
        init_config_recovery_logging();
        LOG_ERROR("[config_recovery] saved config but failed to advance "
                  "last-good snapshot: path=" +
                  nlohmann::json(config_path).dump());
    }
}

void save_config(const AppConfig& cfg, const std::string& explicit_path) {
    fs::path p = path_from_utf8(explicit_path);
    if (p.has_parent_path() && !fs::exists(p.parent_path())) {
        fs::create_directories(p.parent_path());
    }

    const std::string config_path = path_to_utf8(p);
    const std::string bytes = serialize_valid_config(cfg);
    write_validated_config_file(
        config_path, bytes, serialize_mcp_config(cfg.mcp_servers));
    std::string snapshot_error;
    if (!write_last_good_config(config_path, bytes, &snapshot_error)) {
        init_config_recovery_logging();
        LOG_ERROR("[config_recovery] saved config but failed to advance "
                  "last-good snapshot: path=" +
                  nlohmann::json(config_path).dump());
    }
}

bool refresh_default_session_preferences_from_config(
    AppConfig& cfg,
    const std::string& explicit_path,
    std::string* error) {
    if (error) error->clear();

    std::string config_path = explicit_path;
    if (config_path.empty()) {
        config_path = path_to_utf8(path_from_utf8(get_acecode_dir()) / "config.json");
    }

    fs::path native_path = path_from_utf8(config_path);
    std::error_code ec;
    if (!fs::exists(native_path, ec) || ec) {
        return true;
    }

    std::ifstream ifs(native_path);
    if (!ifs.is_open()) {
        if (error) *error = "failed to open config file: " + config_path;
        return false;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(ifs);
    } catch (const std::exception& e) {
        if (error) *error = std::string("failed to parse config file: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        if (error) *error = "config root must be a JSON object";
        return false;
    }

    AppConfig next = cfg;
    if (j.contains("saved_models")) {
        std::string err;
        auto parsed = parse_saved_models(j["saved_models"], err);
        if (!parsed.has_value()) {
            if (error) *error = err;
            return false;
        }
        next.saved_models = std::move(*parsed);
    }
    if (j.contains("default_model_name") && j["default_model_name"].is_string()) {
        next.default_model_name = j["default_model_name"].get<std::string>();
    }
    if (j.contains("default_permission_mode") &&
        j["default_permission_mode"].is_string()) {
        next.default_permission_mode = normalize_permission_mode_name(
            j["default_permission_mode"].get<std::string>());
    } else if (!j.contains("default_permission_mode")) {
        next.default_permission_mode = "default";
    }

    if (!next.saved_models.empty()) {
        std::string err;
        if (!validate_saved_models(next.saved_models, next.default_model_name, err)) {
            if (error) *error = err;
            return false;
        }
        sanitize_disabled_model_providers(next);
    } else if (!next.default_model_name.empty()) {
        LOG_WARN("[config] default_model_name ignored because saved_models is empty: " +
                 next.default_model_name);
        next.default_model_name.clear();
    }

    cfg.saved_models = std::move(next.saved_models);
    cfg.default_model_name = std::move(next.default_model_name);
    cfg.default_permission_mode = std::move(next.default_permission_mode);
    return true;
}

} // namespace acecode
