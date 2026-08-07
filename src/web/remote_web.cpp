#include "remote_web.hpp"

#include "../utils/url_encoding.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace acecode::web {
namespace {

std::string trim_ascii(std::string_view value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::string ascii_lower_copy(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string strip_ipv6_brackets(std::string value) {
    if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

bool parse_ipv4(std::string_view host, std::array<unsigned char, 4>& bytes) {
    in_addr address{};
    const std::string text(host);
#ifdef _WIN32
    if (InetPtonA(AF_INET, text.c_str(), &address) != 1) return false;
#else
    if (inet_pton(AF_INET, text.c_str(), &address) != 1) return false;
#endif
    std::memcpy(bytes.data(), &address, bytes.size());
    return true;
}

bool parse_ipv6(std::string_view host, std::array<unsigned char, 16>& bytes) {
    in6_addr address{};
    const std::string text(host);
#ifdef _WIN32
    if (InetPtonA(AF_INET6, text.c_str(), &address) != 1) return false;
#else
    if (inet_pton(AF_INET6, text.c_str(), &address) != 1) return false;
#endif
    std::memcpy(bytes.data(), &address, bytes.size());
    return true;
}

bool viable_ipv4(const std::array<unsigned char, 4>& bytes) {
    if (bytes[0] == 0 || bytes[0] == 127) return false;
    if (bytes[0] == 169 && bytes[1] == 254) return false;
    if (bytes[0] >= 224) return false;
    return true;
}

bool viable_ipv6(const std::array<unsigned char, 16>& bytes) {
    const bool unspecified = std::all_of(
        bytes.begin(),
        bytes.end(),
        [](unsigned char byte) { return byte == 0; });
    if (unspecified) return false;

    bool loopback = true;
    for (std::size_t i = 0; i < bytes.size() - 1; ++i) {
        loopback = loopback && bytes[i] == 0;
    }
    if (loopback && bytes.back() == 1) return false;

    // fe80::/10 is link-local and cannot be copied without an interface scope.
    if (bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80) return false;
    if (bytes[0] == 0xff) return false; // multicast

    // Reject IPv4-mapped versions of destinations filtered above.
    bool ipv4_mapped = true;
    for (std::size_t i = 0; i < 10; ++i) {
        ipv4_mapped = ipv4_mapped && bytes[i] == 0;
    }
    ipv4_mapped =
        ipv4_mapped && bytes[10] == 0xff && bytes[11] == 0xff;
    if (ipv4_mapped) {
        std::array<unsigned char, 4> mapped{};
        std::copy(bytes.begin() + 12, bytes.end(), mapped.begin());
        return viable_ipv4(mapped);
    }
    return true;
}

bool viable_ip_host(std::string_view raw_host) {
    const std::string host = strip_ipv6_brackets(trim_ascii(raw_host));
    std::array<unsigned char, 4> ipv4{};
    if (parse_ipv4(host, ipv4)) return viable_ipv4(ipv4);
    std::array<unsigned char, 16> ipv6{};
    if (parse_ipv6(host, ipv6)) return viable_ipv6(ipv6);
    return false;
}

bool safe_hostname(std::string_view host) {
    if (host.empty() || host.size() > 253) return false;
    if (host.front() == '.' || host.back() == '.') return false;
    bool label_has_character = false;
    bool previous_hyphen = false;
    std::size_t label_length = 0;
    for (unsigned char ch : host) {
        if (ch == '.') {
            if (!label_has_character || previous_hyphen) return false;
            label_has_character = false;
            previous_hyphen = false;
            label_length = 0;
            continue;
        }
        if (!(std::isalnum(ch) || ch == '-')) return false;
        ++label_length;
        if (label_length > 63) return false;
        if (!label_has_character && ch == '-') return false;
        label_has_character = true;
        previous_hyphen = ch == '-';
    }
    return label_has_character && !previous_hyphen;
}

bool viable_destination_host(std::string_view raw_host) {
    const std::string host = strip_ipv6_brackets(trim_ascii(raw_host));
    std::array<unsigned char, 4> ipv4{};
    if (parse_ipv4(host, ipv4)) return viable_ipv4(ipv4);
    std::array<unsigned char, 16> ipv6{};
    if (parse_ipv6(host, ipv6)) return viable_ipv6(ipv6);
    const std::string lower = ascii_lower_copy(host);
    return safe_hostname(lower) &&
        lower != "localhost" &&
        lower != "localhost.localdomain";
}

bool host_matches_listener_bind(
    std::string_view raw_host,
    std::string_view raw_bind,
    bool allow_hostname) {
    const std::string host = strip_ipv6_brackets(trim_ascii(raw_host));
    const std::string bind = strip_ipv6_brackets(trim_ascii(raw_bind));
    if (bind.empty()) return true;

    std::array<unsigned char, 4> bind_ipv4{};
    if (parse_ipv4(bind, bind_ipv4)) {
        std::array<unsigned char, 4> host_ipv4{};
        if (parse_ipv4(host, host_ipv4)) {
            const bool wildcard = std::all_of(
                bind_ipv4.begin(),
                bind_ipv4.end(),
                [](unsigned char byte) { return byte == 0; });
            return wildcard || host_ipv4 == bind_ipv4;
        }
        std::array<unsigned char, 16> host_ipv6{};
        if (parse_ipv6(host, host_ipv6)) return false;
        return allow_hostname;
    }

    std::array<unsigned char, 16> bind_ipv6{};
    if (parse_ipv6(bind, bind_ipv6)) {
        std::array<unsigned char, 16> host_ipv6{};
        if (parse_ipv6(host, host_ipv6)) {
            const bool wildcard = std::all_of(
                bind_ipv6.begin(),
                bind_ipv6.end(),
                [](unsigned char byte) { return byte == 0; });
            return wildcard || host_ipv6 == bind_ipv6;
        }
        std::array<unsigned char, 4> host_ipv4{};
        if (parse_ipv4(host, host_ipv4)) return false;
        return allow_hostname;
    }

    return allow_hostname;
}

std::optional<std::string> numeric_host(const sockaddr* address) {
    if (!address) return std::nullopt;
    char buffer[INET6_ADDRSTRLEN] = {};
    if (address->sa_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
#ifdef _WIN32
        if (!InetNtopA(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer))) {
#else
        if (!inet_ntop(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer))) {
#endif
            return std::nullopt;
        }
    } else if (address->sa_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
#ifdef _WIN32
        if (!InetNtopA(AF_INET6, &ipv6->sin6_addr, buffer, sizeof(buffer))) {
#else
        if (!inet_ntop(AF_INET6, &ipv6->sin6_addr, buffer, sizeof(buffer))) {
#endif
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    return std::string(buffer);
}

} // namespace

std::optional<std::string> remote_web_host_from_header(
    std::string_view host_header) {
    std::string host = trim_ascii(host_header);
    if (host.empty()) return std::nullopt;

    if (host.front() == '[') {
        const std::size_t close = host.find(']');
        if (close == std::string::npos) return std::nullopt;
        const std::string suffix = host.substr(close + 1);
        if (!suffix.empty()) {
            if (suffix.front() != ':' || suffix.size() == 1) {
                return std::nullopt;
            }
            if (!std::all_of(
                    suffix.begin() + 1,
                    suffix.end(),
                    [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
                return std::nullopt;
            }
        }
        host = host.substr(1, close - 1);
    } else {
        const std::size_t first_colon = host.find(':');
        const std::size_t last_colon = host.rfind(':');
        if (first_colon != std::string::npos && first_colon == last_colon) {
            const std::string port = host.substr(first_colon + 1);
            if (port.empty() ||
                !std::all_of(
                    port.begin(),
                    port.end(),
                    [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
                return std::nullopt;
            }
            host.resize(first_colon);
        }
    }

    host = trim_ascii(host);
    if (!viable_destination_host(host)) return std::nullopt;
    return ascii_lower_copy(host);
}

std::vector<std::string> rank_remote_web_hosts(
    const std::vector<std::string>& discovered_hosts,
    const std::optional<std::string>& preferred_host,
    std::string_view listener_bind,
    const std::optional<std::string>& computer_name) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    const auto append = [&](const std::string& raw, bool allow_hostname) {
        std::string host = strip_ipv6_brackets(trim_ascii(raw));
        const bool viable =
            viable_ip_host(host) ||
            (allow_hostname && viable_destination_host(host));
        if (!viable ||
            !host_matches_listener_bind(
                host,
                listener_bind,
                allow_hostname)) {
            return;
        }
        const std::string key = ascii_lower_copy(host);
        if (seen.insert(key).second) result.push_back(std::move(host));
    };

    if (computer_name) append(*computer_name, true);
    if (preferred_host) append(*preferred_host, true);
    for (const auto& host : discovered_hosts) append(host, false);
    return result;
}

std::vector<std::string> discover_remote_web_hosts() {
    std::vector<std::string> discovered;
#ifdef _WIN32
    ULONG buffer_size = 16 * 1024;
    std::vector<unsigned char> buffer(buffer_size);
    auto* addresses =
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    constexpr ULONG flags =
        GAA_FLAG_SKIP_ANYCAST |
        GAA_FLAG_SKIP_MULTICAST |
        GAA_FLAG_SKIP_DNS_SERVER;
    ULONG status = GetAdaptersAddresses(
        AF_UNSPEC,
        flags,
        nullptr,
        addresses,
        &buffer_size);
    if (status == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        status = GetAdaptersAddresses(
            AF_UNSPEC,
            flags,
            nullptr,
            addresses,
            &buffer_size);
    }
    if (status == NO_ERROR) {
        for (auto* adapter = addresses;
             adapter;
             adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp) continue;
            for (auto* unicast = adapter->FirstUnicastAddress;
                 unicast;
                 unicast = unicast->Next) {
                const auto host = numeric_host(unicast->Address.lpSockaddr);
                if (host) discovered.push_back(*host);
            }
        }
    }
#else
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (const ifaddrs* current = interfaces;
             current;
             current = current->ifa_next) {
            if (!current->ifa_addr ||
                !(current->ifa_flags & IFF_UP)) {
                continue;
            }
            const auto host = numeric_host(current->ifa_addr);
            if (host) discovered.push_back(*host);
        }
        freeifaddrs(interfaces);
    }
#endif
    return rank_remote_web_hosts(discovered);
}

std::optional<std::string> discover_remote_web_computer_name() {
    std::array<char, 256> buffer{};
#ifdef _WIN32
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!GetComputerNameExA(
            ComputerNameDnsHostname,
            buffer.data(),
            &size)) {
        return std::nullopt;
    }
    std::string name(buffer.data(), size);
#else
    if (gethostname(buffer.data(), buffer.size() - 1) != 0) {
        return std::nullopt;
    }
    buffer.back() = '\0';
    std::string name(buffer.data());
#endif
    name = trim_ascii(name);
    if (!viable_destination_host(name)) return std::nullopt;
    return name;
}

std::string build_remote_web_url(
    std::string_view raw_host,
    int port,
    std::string_view token) {
    const std::string host = strip_ipv6_brackets(trim_ascii(raw_host));
    const bool ipv6 = host.find(':') != std::string::npos;
    std::string url = "http://";
    if (ipv6) url += '[';
    url += host;
    if (ipv6) url += ']';
    url += ':' + std::to_string(port);
    url += "/?token=" +
        acecode::utils::percent_encode_query_component(token);
    return url;
}

} // namespace acecode::web
