#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace acecode::utils {

inline bool is_valid_http_base_url(std::string_view url,
                                   bool allow_loopback_http = true) {
    if (url.empty() || url.size() > 2048) return false;
    for (unsigned char c : url) {
        if (c <= 0x20 || c == 0x7f) return false;
    }

    std::string lower(url);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::size_t authority_start = 0;
    bool is_http = false;
    if (lower.rfind("https://", 0) == 0) {
        authority_start = 8;
    } else if (lower.rfind("http://", 0) == 0) {
        authority_start = 7;
        is_http = true;
    } else {
        return false;
    }

    if (lower.find('?', authority_start) != std::string::npos ||
        lower.find('#', authority_start) != std::string::npos) {
        return false;
    }
    const auto authority_end = lower.find('/', authority_start);
    const std::string authority = lower.substr(
        authority_start,
        authority_end == std::string::npos ? std::string::npos
                                           : authority_end - authority_start);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;

    if (is_http) {
        if (!allow_loopback_http) return false;
        const bool loopback = authority == "localhost" ||
            authority.rfind("localhost:", 0) == 0 ||
            authority == "127.0.0.1" || authority.rfind("127.0.0.1:", 0) == 0 ||
            authority == "[::1]" || authority.rfind("[::1]:", 0) == 0;
        if (!loopback) return false;
    }
    return true;
}

} // namespace acecode::utils
