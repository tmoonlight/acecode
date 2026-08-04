#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acecode::web {

inline constexpr const char* kRemoteWebLoopbackBind = "127.0.0.1";
inline constexpr const char* kRemoteWebWildcardBind = "0.0.0.0";

// Remote Web mode intentionally has no second persisted flag. A loopback bind
// is local-only; every other configured bind is treated as remotely reachable.
bool remote_web_enabled_for_bind(std::string_view bind);
std::string remote_web_bind_for_enabled(bool enabled);

// Parses a request Host header into a destination host. The returned value has
// no port or IPv6 brackets and is safe to interpolate into an HTTP URL.
std::optional<std::string> remote_web_host_from_header(
    std::string_view host_header);

// Filters unusable or listener-incompatible destinations, removes duplicates,
// and ranks the current computer name first, followed by the request host and
// then interface addresses. An empty listener_bind accepts either IP address
// family.
std::vector<std::string> rank_remote_web_hosts(
    const std::vector<std::string>& discovered_hosts,
    const std::optional<std::string>& preferred_host = std::nullopt,
    std::string_view listener_bind = {},
    const std::optional<std::string>& computer_name = std::nullopt);

// Returns active non-loopback IP addresses from local network interfaces.
std::vector<std::string> discover_remote_web_hosts();

// Returns the current computer's DNS hostname when it is safe to place in a
// connection URL.
std::optional<std::string> discover_remote_web_computer_name();

// Builds the connection users can open or copy. IPv6 is bracketed and the
// bearer token is encoded as one RFC 3986 query component.
std::string build_remote_web_url(
    std::string_view host,
    int port,
    std::string_view token);

} // namespace acecode::web
