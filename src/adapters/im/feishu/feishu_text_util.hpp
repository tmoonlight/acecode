#pragma once

// 飞书协议实现内部共用的小工具(JSON 字段读取、字符串修剪),不对外暴露。

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace acecode::im::feishu::detail {

inline std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

inline std::string trim(std::string_view text) {
    std::size_t b = 0, e = text.size();
    while (b < e && is_space(text[b])) ++b;
    while (e > b && is_space(text[e - 1])) --e;
    return std::string(text.substr(b, e - b));
}

inline bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

inline bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

inline bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

inline std::string string_field(const nlohmann::json& value, const char* key) {
    if (!value.is_object() || !value.contains(key)) return {};
    const auto& field = value.at(key);
    if (field.is_string()) return field.get<std::string>();
    if (field.is_number_integer()) return std::to_string(field.get<std::int64_t>());
    return {};
}

inline std::int64_t int_field(const nlohmann::json& value, const char* key, std::int64_t fallback) {
    if (!value.is_object() || !value.contains(key)) return fallback;
    const auto& field = value.at(key);
    if (field.is_number_integer()) return field.get<std::int64_t>();
    if (field.is_number_unsigned()) {
        const auto raw = field.get<std::uint64_t>();
        return raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                   ? fallback
                   : static_cast<std::int64_t>(raw);
    }
    if (field.is_number_float()) return static_cast<std::int64_t>(field.get<double>());
    if (field.is_string()) {
        const auto text = trim(field.get<std::string>());
        if (text.empty()) return fallback;
        std::size_t i = text[0] == '-' ? 1 : 0;
        if (i == text.size()) return fallback;
        for (std::size_t k = i; k < text.size(); ++k) {
            if (!is_digit(text[k])) return fallback;
        }
        try {
            return std::stoll(text);
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

inline const nlohmann::json& object_field(const nlohmann::json& value, const char* key) {
    static const nlohmann::json kEmpty = nlohmann::json::object();
    if (!value.is_object() || !value.contains(key) || !value.at(key).is_object()) return kEmpty;
    return value.at(key);
}

} // namespace acecode::im::feishu::detail
