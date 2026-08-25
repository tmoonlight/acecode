#include "model_context_metadata.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace acecode {
namespace {

std::optional<int> positive_int_from_json(const nlohmann::json& value) {
    std::int64_t parsed = 0;
    if (value.is_number_unsigned()) {
        const auto raw = value.get<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            return std::nullopt;
        }
        parsed = static_cast<std::int64_t>(raw);
    } else if (value.is_number_integer()) {
        parsed = value.get<std::int64_t>();
    } else if (value.is_number_float()) {
        const double raw = value.get<double>();
        if (!std::isfinite(raw)) return std::nullopt;
        const double rounded = std::round(raw);
        if (std::fabs(raw - rounded) > 0.000001 || rounded <= 0.0 ||
            rounded > static_cast<double>(std::numeric_limits<int>::max())) {
            return std::nullopt;
        }
        parsed = static_cast<std::int64_t>(rounded);
    } else if (value.is_string()) {
        const std::string raw = value.get<std::string>();
        std::size_t start = 0;
        while (start < raw.size() &&
               std::isspace(static_cast<unsigned char>(raw[start]))) {
            ++start;
        }
        std::size_t end = raw.size();
        while (end > start &&
               std::isspace(static_cast<unsigned char>(raw[end - 1]))) {
            --end;
        }
        if (start == end) return std::nullopt;
        for (std::size_t i = start; i < end; ++i) {
            const unsigned char ch = static_cast<unsigned char>(raw[i]);
            if (!std::isdigit(ch)) return std::nullopt;
            parsed = parsed * 10 + static_cast<std::int64_t>(ch - '0');
            if (parsed > std::numeric_limits<int>::max()) return std::nullopt;
        }
    } else {
        return std::nullopt;
    }

    if (parsed <= 0 || parsed > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

int scan_context_fields(const nlohmann::json& value) {
    if (!value.is_object()) return 0;
    static constexpr const char* kContextKeys[] = {
        "context_window",
        "contextWindow",
        "context_length",
        "contextLength",
        "max_context",
        "maxContext",
        "max_context_length",
        "maxContextLength",
        "max_context_tokens",
        "maxContextTokens",
        "max_input_tokens",
        "maxInputTokens",
        "input_token_limit",
        "inputTokenLimit",
        "input_tokens",
        "max_window_tokens",
        "maxWindowTokens",
        "max_model_len",
        "maxModelLen",
        "n_ctx",
        "context",
    };
    for (const char* key : kContextKeys) {
        const auto it = value.find(key);
        if (it == value.end()) continue;
        if (const auto parsed = positive_int_from_json(*it)) return *parsed;
    }
    return 0;
}

int scan_limit_containers(const nlohmann::json& value) {
    if (!value.is_object()) return 0;
    for (const char* key : {"limit", "limits", "token_limit", "token_limits"}) {
        const auto it = value.find(key);
        if (it == value.end() || !it->is_object()) continue;
        if (const int parsed = scan_context_fields(*it); parsed > 0) return parsed;
        const auto input = it->find("input");
        if (input != it->end()) {
            if (const auto parsed = positive_int_from_json(*input)) return *parsed;
        }
    }
    return 0;
}

} // namespace

int model_context_window_from_metadata(const nlohmann::json& value) {
    if (!value.is_object()) return 0;
    if (const int parsed = scan_context_fields(value); parsed > 0) return parsed;
    if (const int parsed = scan_limit_containers(value); parsed > 0) return parsed;

    for (const char* key : {
             "_meta", "metadata", "meta", "model_info", "modelInfo", "capabilities"}) {
        const auto it = value.find(key);
        if (it == value.end() || !it->is_object()) continue;
        if (const int parsed = scan_context_fields(*it); parsed > 0) return parsed;
        if (const int parsed = scan_limit_containers(*it); parsed > 0) return parsed;
    }
    return 0;
}

} // namespace acecode
