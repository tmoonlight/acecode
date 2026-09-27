#include "mcp_config.hpp"

#include "config_recovery.hpp"
#include "utils/atomic_file.hpp"
#include "utils/cwd_hash.hpp"
#include "utils/logger.hpp"
#include "utils/paths.hpp"
#include "utils/sha256.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>

namespace acecode {
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;
std::recursive_mutex mcp_files_mutex;
constexpr const char* kSpecUrl =
    "https://modelcontextprotocol.io/specification/2026-07-28/schema";

std::string pointer_token(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '~') out += "~0";
        else if (c == '/') out += "~1";
        else out += c;
    }
    return out;
}

void issue(json& errors, const std::string& path, const std::string& message) {
    errors.push_back({{"path", path}, {"message", message}});
}

bool matches_type(const json& value, const std::string& type) {
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "string") return value.is_string();
    if (type == "boolean") return value.is_boolean();
    if (type == "integer") {
        if (value.is_number_integer()) return true;
        if (!value.is_number_float()) return false;
        const auto number = value.get<double>();
        return std::isfinite(number) && std::floor(number) == number;
    }
    return false;
}

// Evaluates the keywords used by our immutable schema. This is deliberately
// not exposed as a general-purpose validator for arbitrary protocol schemas.
void validate(const json& value, const json& schema,
              const std::string& path, json& errors) {
    if (schema.contains("type") &&
        !matches_type(value, schema["type"].get<std::string>())) {
        issue(errors, path, "must be " + schema["type"].get<std::string>());
        return;
    }
    if (schema.contains("enum") &&
        std::find(schema["enum"].begin(), schema["enum"].end(), value) ==
            schema["enum"].end()) {
        issue(errors, path, "must match one of the allowed values");
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (schema.contains("minLength") && text.size() < schema["minLength"].get<size_t>()) {
            issue(errors, path, "must not be empty");
        }
        if (schema.contains("pattern")) {
            const auto pattern = schema["pattern"].get<std::string>();
            const std::regex expression(pattern);
            // MSVC treats ^ as a line anchor even without the multiline flag.
            // Our anchored patterns all consume the entire value; regex_match
            // keeps their semantics consistent with the published JS schema.
            const bool matched = !pattern.empty() && pattern.front() == '^'
                ? std::regex_match(text, expression) : std::regex_search(text, expression);
            if (!matched) issue(errors, path, "must match pattern " + pattern);
        }
    }
    if (value.is_number()) {
        if (schema.contains("minimum") && value < schema["minimum"])
            issue(errors, path, "must be at least " + schema["minimum"].dump());
        if (schema.contains("maximum") && value > schema["maximum"])
            issue(errors, path, "must be at most " + schema["maximum"].dump());
    }
    if (value.is_array() && schema.contains("items")) {
        for (size_t i = 0; i < value.size(); ++i)
            validate(value[i], schema["items"], path + "/" + std::to_string(i), errors);
    }
    if (value.is_object()) {
        if (schema.contains("required")) {
            for (const auto& required : schema["required"]) {
                const auto key = required.get<std::string>();
                if (!value.contains(key)) issue(errors, path + "/" + pointer_token(key), "is required");
            }
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            const auto field_path = path + "/" + pointer_token(it.key());
            if (schema.contains("propertyNames"))
                validate(it.key(), schema["propertyNames"], field_path, errors);
            if (schema.contains("properties") && schema["properties"].contains(it.key())) {
                validate(it.value(), schema["properties"][it.key()], field_path, errors);
            } else if (schema.contains("additionalProperties")) {
                const auto& extra = schema["additionalProperties"];
                if (extra == false) issue(errors, field_path, "unknown field");
                else if (extra.is_object()) validate(it.value(), extra, field_path, errors);
            }
        }
    }
    if (schema.contains("if")) {
        json condition_errors = json::array();
        validate(value, schema["if"], path, condition_errors);
        const char* branch = condition_errors.empty() ? "then" : "else";
        if (schema.contains(branch)) validate(value, schema[branch], path, errors);
    }
}

json error_payload(json errors, bool document) {
    return {{"error", "MCP_CONFIG_INVALID"},
            {"message", "MCP configuration failed schema validation"},
            {"errors", std::move(errors)},
            {"schema", document ? mcp_config_document_schema() : mcp_config_schema()},
            {"specification_url", kSpecUrl}};
}

std::optional<std::string> read_bytes(const std::string& path) {
    std::ifstream input(path_from_utf8(path), std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream bytes;
    bytes << input.rdbuf();
    if (input.bad()) throw std::runtime_error("failed to read MCP configuration");
    return bytes.str();
}

json parse_document(const std::string& bytes) {
    try { return json::parse(bytes); }
    catch (const json::exception&) {
        throw McpConfigError(json::array({{{"path", ""}, {"message", "invalid JSON document"}}}), true);
    }
}

json document_servers(const json& document, bool project) {
    if (project) {
        json errors = json::array();
        validate(document, mcp_config_document_schema(), "", errors);
        if (!errors.empty()) throw McpConfigError(std::move(errors), true);
    } else if (!document.is_object()) {
        throw McpConfigError(json::array({{{"path", ""}, {"message", "must be object"}}}));
    }
    json servers = document.value("mcp_servers", json::object());
    require_valid_mcp_config(servers);
    return servers;
}

json fallback_servers(const std::string& path, const McpConfigError& original,
                      bool allow_full_backup) {
    if (const auto bytes = read_bytes(mcp_last_good_path(path))) {
        try {
            auto servers = json::parse(*bytes);
            require_valid_mcp_config(servers);
            return servers;
        } catch (const std::exception&) { /* Revalidate every recovery candidate. */ }
    }
    if (allow_full_backup) {
        if (const auto bytes = read_last_good_config(path)) {
            try { return document_servers(json::parse(*bytes), false); }
            catch (const std::exception&) { }
        }
    }
    throw original;
}

void archive_or_throw(const std::string& path, const std::string& bytes) {
    std::string error;
    if (!archive_invalid_config(path, bytes, &error))
        throw std::runtime_error("cannot preserve invalid MCP configuration: " + error);
}
} // namespace

const json& mcp_config_schema() {
    static const json schema = json::parse(R"JSON({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "ACECode MCP server configuration",
      "description": "ACECode client configuration, not the MCP protocol message schema.",
      "type": "object",
      "propertyNames": {"minLength": 1, "pattern": "^[^/\\u0000-\\u0020\\u007f]+(?![\\s\\S])"},
      "additionalProperties": {
        "type": "object", "additionalProperties": false,
        "properties": {
          "transport": {"type": "string", "enum": ["stdio", "sse", "http"]},
          "command": {"type": "string", "pattern": "^[^\\u0000\\r\\n]*(?![\\s\\S])"},
          "args": {"type": "array", "items": {"type": "string", "pattern": "^[^\\u0000]*(?![\\s\\S])"}},
          "env": {"type": "object", "propertyNames": {"minLength": 1, "pattern": "^[^=\\u0000]+(?![\\s\\S])"}, "additionalProperties": {"type": "string", "pattern": "^[^\\u0000]*(?![\\s\\S])"}},
          "url": {"type": "string"},
          "sse_endpoint": {"type": "string", "pattern": "^/[^\\s#]*(?![\\s\\S])"},
          "headers": {"type": "object", "propertyNames": {"minLength": 1, "pattern": "^[!#$%&'*+.^_`|~0-9A-Za-z-]+(?![\\s\\S])"}, "additionalProperties": {"type": "string", "pattern": "^[^\\u0000\\r\\n]*(?![\\s\\S])"}},
          "auth_token": {"type": "string", "pattern": "^[^\\u0000\\r\\n]*(?![\\s\\S])"},
          "timeout_seconds": {"type": "integer", "minimum": 1, "maximum": 3600},
          "disabled": {"type": "boolean"},
          "validate_certificates": {"type": "boolean", "description": "Deprecated and ignored; TLS verification remains enabled."},
          "ca_cert_path": {"type": "string", "description": "Deprecated and ignored."}
        },
        "if": {"required": ["transport"], "properties": {"transport": {"enum": ["sse", "http"]}}},
        "then": {"required": ["url"], "properties": {"url": {"pattern": "^https?://[^\\s/?#@]+(?:[/?][^\\s#]*)?(?![\\s\\S])"}}},
        "else": {"required": ["command"], "properties": {"command": {"minLength": 1, "pattern": "\\S"}}}
      }
    })JSON");
    return schema;
}

json mcp_config_document_schema() {
    return {{"$schema", "http://json-schema.org/draft-07/schema#"},
            {"title", "ACECode project MCP configuration"},
            {"type", "object"}, {"additionalProperties", false},
            {"required", json::array({"mcp_servers"})},
            {"properties", {{"mcp_servers", mcp_config_schema()},
                             {"$schema", {{"type", "string"}}}}}};
}

json validate_mcp_config(const json& servers) {
    json errors = json::array();
    validate(servers, mcp_config_schema(), "", errors);
    return errors;
}

McpConfigError::McpConfigError(json errors, bool document)
    : std::runtime_error(error_payload(errors, document).dump()),
      payload_(error_payload(std::move(errors), document)) {}

void require_valid_mcp_config(const json& servers) {
    auto errors = validate_mcp_config(servers);
    if (!errors.empty()) throw McpConfigError(std::move(errors));
}

McpServerMap parse_mcp_config(const json& servers, const McpServerMap* preserve_hidden) {
    require_valid_mcp_config(servers);
    McpServerMap result;
    for (auto it = servers.begin(); it != servers.end(); ++it) {
        const auto& value = it.value();
        McpServerConfig cfg;
        const auto transport = value.value("transport", std::string("stdio"));
        cfg.transport = transport == "sse" ? McpTransport::Sse
            : transport == "http" ? McpTransport::Http : McpTransport::Stdio;
        cfg.command = value.value("command", std::string{});
        cfg.args = value.value("args", std::vector<std::string>{});
        cfg.env = value.value("env", std::map<std::string, std::string>{});
        cfg.url = value.value("url", std::string{});
        cfg.sse_endpoint = value.value("sse_endpoint", std::string(transport == "http" ? "/mcp" : "/sse"));
        cfg.headers = value.value("headers", std::map<std::string, std::string>{});
        cfg.auth_token = value.value("auth_token", std::string{});
        if (!value.contains("auth_token") && preserve_hidden) {
            const auto old = preserve_hidden->find(it.key());
            if (old != preserve_hidden->end()) cfg.auth_token = old->second.auth_token;
        }
        cfg.timeout_seconds = value.value("timeout_seconds", 30);
        cfg.disabled = value.value("disabled", false);
        result.emplace(it.key(), std::move(cfg));
    }
    return result;
}

json serialize_mcp_config(const McpServerMap& servers, bool include_auth_token) {
    json result = json::object();
    for (const auto& [name, cfg] : servers) {
        json value = {{"transport", cfg.transport == McpTransport::Stdio ? "stdio"
            : cfg.transport == McpTransport::Sse ? "sse" : cfg.transport == McpTransport::Http ? "http" : "invalid"}};
        if (!cfg.command.empty()) value["command"] = cfg.command;
        if (!cfg.args.empty()) value["args"] = cfg.args;
        if (!cfg.env.empty()) value["env"] = cfg.env;
        if (!cfg.url.empty()) value["url"] = cfg.url;
        value["sse_endpoint"] = cfg.sse_endpoint;
        if (!cfg.headers.empty()) value["headers"] = cfg.headers;
        if (include_auth_token && !cfg.auth_token.empty()) value["auth_token"] = cfg.auth_token;
        value["timeout_seconds"] = cfg.timeout_seconds;
        if (cfg.disabled) value["disabled"] = true;
        result[name] = std::move(value);
    }
    return result;
}

std::string mcp_project_root(const std::string& cwd) {
    if (cwd.empty()) throw std::invalid_argument("project MCP requires a workspace");
    std::error_code ec;
    fs::path start = fs::weakly_canonical(fs::absolute(path_from_utf8(cwd)), ec);
    if (ec) throw std::runtime_error("cannot resolve project MCP workspace");
#ifdef _WIN32
    start = path_from_utf8(normalize_cwd_for_hash(path_to_utf8(start)));
#endif
    // Match project instructions/skills: the user's home is the global
    // boundary even when it is itself a Git repository (e.g. dotfiles).
    for (const auto& directory : get_project_dirs_up_to_home(path_to_utf8(start))) {
        const auto current = path_from_utf8(directory);
        if (fs::exists(current / ".acecode" / "mcp.json") || fs::exists(current / ".git"))
            return path_to_utf8(current);
    }
    return path_to_utf8(start);
}

std::string mcp_project_config_path(const std::string& cwd) {
    return path_to_utf8(path_from_utf8(mcp_project_root(cwd)) / ".acecode" / "mcp.json");
}

std::string mcp_project_server_id(const std::string& cwd, const std::string& name) {
    // Slash is forbidden in configured names, reserving this owner namespace.
    return "project/" + sha256_hex(mcp_project_root(cwd)).substr(0, 16) + "/" + name;
}

std::string mcp_server_display_name(const std::string& id) {
    if (id.rfind("project/", 0) != 0) return id;
    const auto end = id.find('/', 8);
    return end == std::string::npos ? id : id.substr(end + 1);
}

std::string mcp_last_good_path(const std::string& config_path) {
    return config_path + ".mcp-last-good";
}

void remember_valid_mcp_config(const std::string& path, const json& servers) {
    require_valid_mcp_config(servers);
    std::lock_guard<std::recursive_mutex> lock(mcp_files_mutex);
    const auto bytes = servers.dump(2) + "\n";
    if (read_bytes(mcp_last_good_path(path)) == bytes) return;
    if (!atomic_write_file(mcp_last_good_path(path), bytes, true))
        throw std::runtime_error("failed to save MCP last-good snapshot");
}

void write_validated_config_file(const std::string& path, const std::string& bytes,
                                 const json& servers) {
    require_valid_mcp_config(servers);
    std::lock_guard<std::recursive_mutex> lock(mcp_files_mutex);
    const auto previous = read_bytes(path);
    if (!atomic_write_file(path, bytes, true))
        throw std::runtime_error("failed to write configuration file");
    try { remember_valid_mcp_config(path, servers); }
    catch (...) {
        if (previous) {
            if (!atomic_write_file(path, *previous, true))
                throw std::runtime_error("failed to save MCP snapshot and restore configuration");
        } else {
            std::error_code ec;
            fs::remove(path_from_utf8(path), ec);
            if (ec) throw std::runtime_error("failed to save MCP snapshot and remove new configuration");
        }
        throw;
    }
}

bool recover_mcp_config(json& document, const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(mcp_files_mutex);
    try {
        remember_valid_mcp_config(path, document_servers(document, false));
        return false;
    } catch (const McpConfigError& error) {
        const auto servers = fallback_servers(path, error, true);
        auto restored = document;
        restored["mcp_servers"] = servers;
        archive_or_throw(path, read_bytes(path).value_or(document.dump()));
        write_validated_config_file(path, restored.dump(2) + "\n", servers);
        document = std::move(restored);
        LOG_WARN("[mcp] restored last-good global MCP configuration");
        return true;
    }
}

McpServerMap load_project_mcp_config(const std::string& cwd) {
    std::lock_guard<std::recursive_mutex> lock(mcp_files_mutex);
    const auto path = mcp_project_config_path(cwd);
    const auto bytes = read_bytes(path);
    if (!bytes) {
        if (fs::exists(path_from_utf8(path))) throw std::runtime_error("cannot read project MCP configuration");
        return {};
    }
    try {
        const auto servers = document_servers(parse_document(*bytes), true);
        remember_valid_mcp_config(path, servers);
        return parse_mcp_config(servers);
    } catch (const McpConfigError& error) {
        const auto servers = fallback_servers(path, error, false);
        archive_or_throw(path, *bytes);
        write_validated_config_file(path, json{{"mcp_servers", servers}}.dump(2) + "\n", servers);
        LOG_WARN("[mcp] restored last-good project MCP configuration");
        return parse_mcp_config(servers);
    }
}

void save_project_mcp_config(const std::string& cwd, const json& servers) {
    require_valid_mcp_config(servers);
    write_validated_config_file(mcp_project_config_path(cwd),
        json{{"mcp_servers", servers}}.dump(2) + "\n", servers);
}

McpServerMap effective_mcp_config(const McpServerMap& global, const McpServerMap& project) {
    auto effective = global;
    for (const auto& [name, cfg] : project) effective[name] = cfg;
    return effective;
}

std::optional<json> validate_mcp_file_edit(const std::string& path, const std::string& content) {
    auto native = fs::weakly_canonical(fs::absolute(path_from_utf8(path)));
    auto global_path = fs::weakly_canonical(path_from_utf8(get_acecode_dir()) / "config.json");
#ifdef _WIN32
    native = path_from_utf8(normalize_cwd_for_hash(path_to_utf8(native)));
    global_path = path_from_utf8(normalize_cwd_for_hash(path_to_utf8(global_path)));
#endif
    const auto filename = path_to_utf8(native.filename());
    const bool project = filename == "mcp.json" && native.parent_path().filename() == ".acecode";
    const bool global = filename == "config.json" &&
        (native.parent_path().filename() == ".acecode" || native == global_path);
    if (!project && !global) return std::nullopt;
    return document_servers(parse_document(content), project);
}
} // namespace acecode
