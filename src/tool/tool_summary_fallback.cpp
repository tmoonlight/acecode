#include "tool_executor.hpp"

#include "utils/encoding.hpp"

#include <string>
#include <unordered_set>
#include <vector>

namespace acecode {

namespace {

constexpr size_t kFallbackArgumentPreviewBytes = 80;
constexpr size_t kFallbackObjectPreviewBytes = 240;

std::string capitalize_tool_name(const std::string& tool_name) {
    std::string result = tool_name.empty() ? "Tool" : tool_name;
    if (result[0] >= 'a' && result[0] <= 'z') {
        result[0] = static_cast<char>(result[0] - 'a' + 'A');
    }
    return result;
}

std::string collapse_ascii_whitespace(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    bool pending_space = false;
    for (const unsigned char ch : value) {
        const bool whitespace =
            ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
            ch == '\f' || ch == '\v';
        if (whitespace) {
            pending_space = !result.empty();
            continue;
        }
        if (pending_space) result.push_back(' ');
        result.push_back(static_cast<char>(ch));
        pending_space = false;
    }
    return result;
}

std::string normalized_key(const std::string& key) {
    std::string result;
    result.reserve(key.size());
    for (const unsigned char ch : key) {
        if (ch >= 'A' && ch <= 'Z') {
            result.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else if ((ch >= 'a' && ch <= 'z') ||
                   (ch >= '0' && ch <= '9')) {
            result.push_back(static_cast<char>(ch));
        }
    }
    return result;
}

bool is_sensitive_key(const std::string& key) {
    static const std::unordered_set<std::string> sensitive_keys = {
        "apikey",
        "authorization",
        "authtoken",
        "accesstoken",
        "refreshtoken",
        "bearertoken",
        "clientsecret",
        "credential",
        "credentials",
        "password",
        "passwd",
        "privatekey",
        "secret",
        "token",
    };
    const std::string normalized = normalized_key(key);
    if (sensitive_keys.count(normalized) != 0) return true;
    const auto ends_with = [&](const std::string& suffix) {
        return normalized.size() >= suffix.size() &&
            normalized.compare(
                normalized.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return ends_with("apikey") ||
        ends_with("authorization") ||
        ends_with("credential") ||
        ends_with("credentials") ||
        ends_with("password") ||
        ends_with("passwd") ||
        ends_with("privatekey") ||
        ends_with("secret") ||
        ends_with("token");
}

nlohmann::ordered_json redact_sensitive_values(
    const nlohmann::ordered_json& value) {
    if (value.is_object()) {
        nlohmann::ordered_json result = nlohmann::ordered_json::object();
        for (auto it = value.begin(); it != value.end(); ++it) {
            result[it.key()] = is_sensitive_key(it.key())
                ? nlohmann::ordered_json("[REDACTED]")
                : redact_sensitive_values(it.value());
        }
        return result;
    }
    if (value.is_array()) {
        nlohmann::ordered_json result = nlohmann::ordered_json::array();
        for (const auto& item : value) {
            result.push_back(redact_sensitive_values(item));
        }
        return result;
    }
    return value;
}

std::string argument_preview(const nlohmann::ordered_json& value) {
    std::string preview;
    if (value.is_string()) {
        preview = collapse_ascii_whitespace(value.get<std::string>());
    } else {
        preview = redact_sensitive_values(value).dump();
    }
    return truncate_utf8_prefix(preview, kFallbackArgumentPreviewBytes);
}

std::string join_argument_previews(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) result += " \xC2\xB7 ";
        result += value;
    }
    return truncate_utf8_prefix(result, kFallbackObjectPreviewBytes);
}

} // namespace

ToolSummary build_fallback_tool_summary(
    const std::string& tool_name,
    const std::string& arguments_json) {
    ToolSummary summary;
    summary.verb = capitalize_tool_name(tool_name);
    summary.icon = "*";

    if (arguments_json.empty()) return summary;

    auto arguments = nlohmann::ordered_json::parse(
        arguments_json, nullptr, false);
    if (arguments.is_discarded()) {
        summary.object = truncate_utf8_prefix(
            collapse_ascii_whitespace(arguments_json),
            kFallbackObjectPreviewBytes);
        return summary;
    }

    std::vector<std::string> values;
    if (arguments.is_object()) {
        values.reserve(arguments.size());
        for (auto it = arguments.begin(); it != arguments.end(); ++it) {
            if (is_sensitive_key(it.key())) {
                values.push_back("[REDACTED]");
            } else {
                values.push_back(argument_preview(it.value()));
            }
        }
    } else if (!arguments.is_null()) {
        values.push_back(argument_preview(arguments));
    }
    summary.object = join_argument_previews(values);
    return summary;
}

void ensure_tool_summary(
    const std::string& tool_name,
    const std::string& arguments_json,
    ToolResult& result) {
    if (!result.summary.has_value()) {
        result.summary = build_fallback_tool_summary(
            tool_name, arguments_json);
    }
}

} // namespace acecode
