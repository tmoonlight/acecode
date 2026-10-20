#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace acecode::desktop {

// Only native navigation can establish trust. Normalize the origin, never keep
// the daemon token/query, and compare ports as well as the loopback hostname.
inline std::string desktop_microphone_origin(std::string_view url) {
    for (unsigned char ch : url) {
        if (ch <= 0x20 || ch == 0x7f || ch == '\\') return {};
    }
    const auto separator = url.find("://");
    if (separator == std::string_view::npos) return {};
    const auto end = url.find_first_of("/?#", separator + 3);
    std::string origin(url.substr(0, end));
    std::transform(origin.begin(), origin.end(), origin.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const bool https = origin.rfind("https://", 0) == 0;
    if (!https && origin.rfind("http://", 0) != 0) return {};
    const auto scheme_end = https ? 8u : 7u;
    const std::string_view authority(origin.data() + scheme_end, origin.size() - scheme_end);
    if (authority.empty()) return {};
    const auto host_end = authority.front() == '[' ? authority.find(']') + 1 : authority.find(':');
    const auto host = authority.substr(0, host_end);
    if (host != "127.0.0.1" && host != "localhost" && host != "[::1]") return {};
    unsigned int port = https ? 443 : 80;
    if (host.size() < authority.size()) {
        const auto suffix = authority.substr(host.size());
        if (suffix.size() < 2 || suffix.front() != ':') return {};
        port = 0;
        for (const char ch : suffix.substr(1)) {
            if (ch < '0' || ch > '9') return {};
            port = port * 10 + static_cast<unsigned int>(ch - '0');
            if (port > 65535) return {};
        }
        if (!port) return {};
    }
    return origin.substr(0, scheme_end) + std::string(host) + ':' + std::to_string(port);
}

inline bool may_grant_desktop_microphone(const std::string& trusted_origin,
                                         std::string_view current_url,
                                         std::string_view request_url) {
    return !trusted_origin.empty() &&
        desktop_microphone_origin(current_url) == trusted_origin &&
        desktop_microphone_origin(request_url) == trusted_origin;
}

// WebKitGTK capture requests: audio-only capture from the page native code
// navigated to is allowed; everything else is denied. Other permission kinds
// keep WebKit's default handling.
struct LinuxMediaPermissionRequest {
    bool user_media = false;
    bool audio = false;
    bool video = false;
};

enum class LinuxMediaPermissionDecision { Unhandled, Allow, Deny };

inline LinuxMediaPermissionDecision decide_linux_media_permission(
        const std::string& trusted_origin, std::string_view current_url,
        const LinuxMediaPermissionRequest& request) {
    if (!request.user_media) return LinuxMediaPermissionDecision::Unhandled;
    const bool allowed = request.audio && !request.video &&
        may_grant_desktop_microphone(trusted_origin, current_url, current_url);
    return allowed ? LinuxMediaPermissionDecision::Allow : LinuxMediaPermissionDecision::Deny;
}

} // namespace acecode::desktop
