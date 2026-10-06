#include "channels_handler.hpp"

#include "channels/core/platforms.hpp"

#include <vector>

namespace acecode::web {
namespace {

constexpr std::size_t kMaxCredentialBytes = 512;
constexpr std::size_t kMaxPrincipalBytes = 300;

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

bool is_channel_platform(const std::string& platform) {
    return channels::core::find_platform_spec(platform) != nullptr;
}

bool parse_channel_enabled_request(const nlohmann::json& body, bool& enabled, std::string& error) {
    if (!body.is_object() || !body.contains("enabled") || !body["enabled"].is_boolean()) {
        error = "请求体需要 {\"enabled\": true 或 false}";
        return false;
    }
    enabled = body["enabled"].get<bool>();
    return true;
}

std::optional<nlohmann::json> parse_channel_credentials_request(const std::string& platform,
                                                                 const nlohmann::json& body,
                                                                 std::string& error) {
    if (!body.is_object()) {
        error = "请求体必须是 JSON 对象";
        return std::nullopt;
    }
    const auto* spec = channels::core::find_platform_spec(platform);
    if (!spec) {
        error = "未知的消息通道平台";
        return std::nullopt;
    }
    nlohmann::json credentials = nlohmann::json::object();
    for (const auto& [key, value] : body.items()) {
        bool known = false;
        for (const auto& field : spec->fields) known = known || (field.user_input && field.key == key);
        if (!known) {
            error = "不支持的字段:" + key;
            return std::nullopt;
        }
        if (!value.is_string()) {
            error = "字段 " + key + " 必须是字符串";
            return std::nullopt;
        }
        const auto text = trim(value.get<std::string>());
        if (text.size() > kMaxCredentialBytes || text.find('\0') != std::string::npos) {
            error = "字段 " + key + " 过长或包含非法字符";
            return std::nullopt;
        }
        if (!text.empty()) {
            credentials[key] = text;
            continue;
        }
        // 可选字段允许显式提交空串表示清除;必填字段的空串表示沿用已保存的值,直接略过。
        for (const auto& field : spec->fields)
            if (field.key == key && !field.required) credentials[key] = "";
    }
    if (credentials.empty()) {
        error = "请填写凭据";
        return std::nullopt;
    }
    return credentials;
}

std::optional<std::string> decode_channel_principal(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%') {
            if (i + 2 >= raw.size()) return std::nullopt;
            const int hi = hex_digit(raw[i + 1]);
            const int lo = hex_digit(raw[i + 2]);
            if (hi < 0 || lo < 0) return std::nullopt;
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(raw[i]);
        }
    }
    if (out.empty() || out.size() > kMaxPrincipalBytes || out.find('\0') != std::string::npos) return std::nullopt;
    const bool known_prefix =
        out.rfind("user:", 0) == 0 || out.rfind("group:", 0) == 0 || out.rfind("member:", 0) == 0;
    if (!known_prefix) return std::nullopt;
    return out;
}

nlohmann::json channel_error_body(const std::string& code, const std::string& message) {
    return {{"error", code}, {"message", message}};
}

void annotate_channel_bound(nlohmann::json& rows, const std::map<std::string, std::string>& bound) {
    if (!rows.is_array() || bound.empty()) return;
    for (auto& row : rows) {
        if (!row.is_object() || !row.contains("id") || !row["id"].is_string()) continue;
        const auto it = bound.find(row["id"].get<std::string>());
        if (it != bound.end()) row["channel_bound"] = {{"platform", it->second}};
    }
}

} // namespace acecode::web
