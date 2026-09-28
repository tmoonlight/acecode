#pragma once

#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace acecode::utils {

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
