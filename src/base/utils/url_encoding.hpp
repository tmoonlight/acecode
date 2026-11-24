#pragma once

#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace acecode::utils {

inline int percent_hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

inline std::optional<std::string> percent_decode_path_segment(std::string_view raw) {
    std::string output;
    output.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%') {
            if (i + 2 >= raw.size()) return std::nullopt;
            const int hi = percent_hex_digit(raw[i + 1]);
            const int lo = percent_hex_digit(raw[i + 2]);
            if (hi < 0 || lo < 0) return std::nullopt;
            const char decoded = static_cast<char>((hi << 4) | lo);
            if (decoded == '\0') return std::nullopt;
            output.push_back(decoded);
            i += 2;
        } else {
            if (raw[i] == '\0') return std::nullopt;
            output.push_back(raw[i]);
        }
    }
    return output;
}

inline std::string percent_encode_query_component(std::string_view raw) {
    const auto is_unreserved = [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '.' || ch == '_' || ch == '~';
    };

    std::ostringstream output;
    output.fill('0');
    output << std::hex << std::uppercase;
    for (unsigned char ch : raw) {
        if (is_unreserved(ch)) {
            output << static_cast<char>(ch);
        } else {
            output << '%' << std::setw(2) << static_cast<int>(ch);
        }
    }
    return output.str();
}

} // namespace acecode::utils
