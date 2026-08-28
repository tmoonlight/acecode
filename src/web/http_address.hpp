#pragma once

#include <string>
#include <string_view>

namespace acecode::web {

inline std::string format_http_address(std::string_view host, int port) {
    std::string url = "http://";
    const bool needs_ipv6_brackets =
        host.find(':') != std::string_view::npos &&
        !(host.size() >= 2 && host.front() == '[' && host.back() == ']');
    if (needs_ipv6_brackets) url.push_back('[');
    url.append(host.data(), host.size());
    if (needs_ipv6_brackets) url.push_back(']');
    url.push_back(':');
    url += std::to_string(port);
    url.push_back('/');
    return url;
}

} // namespace acecode::web
